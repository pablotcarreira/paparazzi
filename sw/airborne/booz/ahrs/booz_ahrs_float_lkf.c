/*
 * $Id:  $
 *  
 * Copyright (C) 2009 Felix Ruess <felix.ruess@gmail.com>
 * Copyright (C) 2009 Antoine Drouin <poinix@gmail.com>
 *
 * This file is part of paparazzi.
 *
 * paparazzi is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * paparazzi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with paparazzi; see the file COPYING.  If not, write to
 * the Free Software Foundation, 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA. 
 */

#include "booz_ahrs_float_lkf.h"

#include "booz_imu.h"
#include "booz_ahrs_aligner.h"

#include "airframe.h"
#include "math/pprz_algebra_float.h"

struct BoozAhrs booz_ahrs;


/* our estimated attitude                     */
struct FloatQuat  bafl_quat;
/* our estimated gyro biases                  */
struct FloatRates bafl_bias;
/* our estimated attitude errors              */
struct FloatQuat bafl_quat_err;
/* our estimated gyro bias errors             */
struct FloatRates bafl_bias_err;
/* we get unbiased body rates as byproduct    */
struct FloatRates bafl_rates;
/* maintain a euler angles representation     */
struct FloatEulers bafl_eulers;
/* maintains a rotation matrix representation */
struct FloatRMat bafl_dcm;
/* time derivative of our quaternion */
struct FloatQuat bafl_qdot;

#define BAFL_SSIZE 6
/* error covariance matrix */
float bafl_P[BAFL_SSIZE][BAFL_SSIZE];
/* filter state */
float bafl_X[BAFL_SSIZE];

/*
 * F represents the Jacobian of the derivative of the system with respect
 * its states.  We do not allocate the bottom three rows since we know that
 * the derivatives of bias_dot are all zero.
 */
float bafl_F[3][3];
/*
 * T represents the discrete state transition matrix
 * T = e^(F * dt)
 */
float bafl_T[6][6];


/*
 * Kalman filter variables.
 */
float bafl_Pprio[BAFL_SSIZE][BAFL_SSIZE];
/* temporary variable used during state covariance update */
float bafl_tempP[BAFL_SSIZE][BAFL_SSIZE];
float bafl_K[6][3];
float bafl_tempK[6][3];
float bafl_S[3][3];
float bafl_tempS[3][6];
float bafl_invS[3][3];
struct FloatVect3 bafl_y;

/*
 * H represents the Jacobian of the measurements of the attitude
 * with respect to the states of the filter.  We do not allocate the bottom
 * three rows since we know that the attitude measurements have no
 * relationship to gyro bias.
 */
float bafl_H[3][6];

struct FloatVect3 bafl_accel;
struct FloatVect3 bafl_mag;

/* temporary variables for the strapdown computation */
float bafl_qom[4][4];

#define bafl_g 9.81
#define bafl_h {236.0, -2.0, 396.0}

/*
 * Q is our estimate noise variance.  It is supposed to be an NxN
 * matrix, but with elements only on the diagonals.  Additionally,
 * since the quaternion has no expected noise (we can't directly measure
 * it), those are zero.  For the gyro, we expect around 5 deg/sec noise,
 * which is 0.08 rad/sec.  The variance is then 0.08^2 ~= 0.0075.
 */
/* I measured about 0.009 rad/s noise */
#define bafl_Q_gyro 8e-03

/*
 * R is our measurement noise estimate.  Like Q, it is supposed to be
 * an NxN matrix with elements on the diagonals.  However, since we can
 * not directly measure the gyro bias, we have no estimate for it.
 * We only have an expected noise in the pitch and roll accelerometers
 * and in the compass.
 */
#define BAFL_R_PHI   1.3 * 1.3
#define BAFL_R_THETA 1.3 * 1.3
#define BAFL_R_PSI   2.5 * 2.5

#define BAFL_R_ACCEL 5.0  * 5.0
#define BAFL_R_MAG   300. * 300.

#ifndef BAFL_DT
#define BAFL_DT (1./512.)
#endif

void booz_ahrs_init(void) {
  int i, j, k;

  for(i = 0; i < BAFL_SSIZE; i++) {
	for(k = 0; k < BAFL_SSIZE; k++) {
		bafl_T[i][k] = 0.;
	}
	for(j = 0; j < 3; j++) {
		bafl_H[j][i] = 0.;
	}
		
	/* Insert the diagonal elements of the T-Matrix. These will never change. */
	bafl_T[i][i]=1.0;
  }

  FLOAT_QUAT_ZERO(bafl_quat);

  booz_ahrs.status = BOOZ_AHRS_UNINIT;
  INT_EULERS_ZERO(booz_ahrs.ltp_to_body_euler);
  INT_EULERS_ZERO(booz_ahrs.ltp_to_imu_euler);
  INT32_QUAT_ZERO(booz_ahrs.ltp_to_body_quat);
  INT32_QUAT_ZERO(booz_ahrs.ltp_to_imu_quat);
  INT_RATES_ZERO(booz_ahrs.body_rate);
  INT_RATES_ZERO(booz_ahrs.imu_rate);

}

void booz_ahrs_align(void) {
  RATES_FLOAT_OF_BFP(bafl_bias, booz_ahrs_aligner.lp_gyro);
  booz_ahrs.status = BOOZ_AHRS_RUNNING;
}


/* 
 * Propagate our dynamic system in time
 *
 * Run the strapdown computation and the predict step of the filter
 *
 *
 *      quat_dot = Wxq(pqr) * quat
 *      bias_dot = 0
 *
 * Wxq is the quaternion omega matrix:
 *
 *              [ 0, -p, -q, -r ]
 *      1/2 *   [ p,  0,  r, -q ]
 *              [ q, -r,  0,  p ]
 *              [ r,  q, -p,  0 ]
 *
 */
void booz_ahrs_propagate(void) {
  int i,j,k;
	
  /* compute unbiased rates */
  RATES_FLOAT_OF_BFP(bafl_rates, booz_imu.gyro);
  RATES_SUB(bafl_rates, bafl_bias);
  
  
  /* run strapdown computation 
   *
   */
   
  /* compute qdot and normalize it */
  FLOAT_QUAT_DERIVATIVE_LAGRANGE(bafl_qdot, bafl_rates, bafl_quat);
  
  /* multiply qdot with dt */
  QUAT_SMUL(bafl_qdot, bafl_qdot, BAFL_DT);
  /* propagate quaternion */
  QUAT_ADD(bafl_quat, bafl_qdot);
  /* maintain rotation matrix representation */
  FLOAT_RMAT_OF_QUAT(bafl_dcm, bafl_quat);
  
  //TODO check if normalization is good
  
  
  
  
  /* error KF-Filter
   * propagage only the error covariance matrix since error is corrected after each measurement
   * 
   * F = [ 0  0  0          ]
   *     [ 0  0  0   -Cbn   ]
   *     [ 0  0  0          ]
   *     [ 0  0  0  0  0  0 ]
   *     [ 0  0  0  0  0  0 ]
   *     [ 0  0  0  0  0  0 ]
   *
   * T = e^(dt * F)  
   *
   * P_prio = T * P * T_T + Q
   *
   */
  
  /* 
   *  compute state transition matrix T
   */
  for (i=0; i<3; i++) {
    for (j=0; j<3; j++) {
	  bafl_T[i][j+3] = - RMAT_ELMT(bafl_dcm, j, i); /* inverted bafl_dcm */
	}
  }

  
  /*
   * estimate the a priori error covariance matrix P_prio = T * P * T_T + Q
   */
  /* Temporary multiplication temp = T * P      */
  for (i=0; i<BAFL_SSIZE; i++) {
	for (j=0; j<BAFL_SSIZE; j++) {
	  bafl_tempP[i][j] = 0.;
	  for(k=0; k<BAFL_SSIZE; k++) {
		bafl_tempP[i][j] += bafl_T[i][k] * bafl_P[k][j];
	  }
	}
  }
  
  // P = temp * T_T + Q
  for ( i=0; i<BAFL_SSIZE; i++ ) {
	for ( j=0; j<BAFL_SSIZE; j++ ) {
	  if (i >= 3 && i == j) {
		bafl_P[i][j] = bafl_Q_gyro;
	  } else {
		bafl_P[i][j] = 0.;
	  }
	  for(k=0; k < BAFL_SSIZE; k++) {
		bafl_P[i][j] += bafl_tempP[i][k] * bafl_T[j][k]; //T[j][k] = T_T[k][j]
	  }
	}
  }

}


void booz_ahrs_update_accel(void) {
  int i, j, k;
  
  
  ACCELS_FLOAT_OF_BFP(bafl_accel, booz_imu.accel);
  
  /* P_prio = P */
  for ( i=0; i<BAFL_SSIZE; i++ ) {
	for ( j=0; j<BAFL_SSIZE; j++ ) {
	  bafl_Pprio[i][j] = bafl_P[i][j];
	}
  }
  
  /* set up measurement matrix */ 
  bafl_H[0][0] = -RMAT_ELMT(bafl_dcm, 0, 1) * bafl_g;
  bafl_H[0][1] =  RMAT_ELMT(bafl_dcm, 0, 0) * bafl_g;
  bafl_H[1][0] = -RMAT_ELMT(bafl_dcm, 1, 1) * bafl_g;
  bafl_H[1][1] =  RMAT_ELMT(bafl_dcm, 1, 0) * bafl_g;
  bafl_H[2][0] = -RMAT_ELMT(bafl_dcm, 2, 1) * bafl_g;
  bafl_H[2][1] =  RMAT_ELMT(bafl_dcm, 2, 0) * bafl_g;
  //rest is zero
  
  
  
  
  
  /**********************************************
   * compute Kalman gain K
   * 
   * K = P_prio * H_T * inv(S)
   * S = H * P_prio * HT + R
   *
   * K = P_prio * HT * inv(H * P_prio * HT + R)
   *
   **********************************************/
  
  
  /* covariance residual S = H * P_prio * HT + R    */
  
  /* temp_S(3x6) = H(3x6) * P_prio(6x6)
   * last 3 columns of H are zero
   */
  for (i=0; i<3; i++) {
	for (j=0; j<6; j++) {
	  bafl_tempS[i][j] = bafl_H[i][0] * bafl_Pprio[0][j];
	  for (k=1; k<3; k++) {
		bafl_tempS[i][j] +=  bafl_H[i][k] * bafl_Pprio[k][j];
	  }
	}
  }

  
  /* S(3x3) = temp_S(3x6) * HT(6x3) + R(3x3)
   *
   * last 3 rows of HT are zero
   */
  for (i=0; i<3; i++) {
	for (j=0; j<3; j++) {
	  if (i < 3 && i == j) {
		bafl_S[i][j] = BAFL_R_ACCEL;
	  } else {
		bafl_S[i][j] = 0.;
	  }
	  for (k=0; k<3; k++) {		/* normally k<6, not computing zero elements */
		bafl_S[i][j] +=  bafl_tempS[i][k] * bafl_H[j][k]; /* H[j][k] = HT[k][j] */
	  }
	}
  }
  
  /* invert S 
   */
  float detS = 0.;
  detS = bafl_S[0][0]*(bafl_S[2][2]*bafl_S[1][1]-bafl_S[2][1]*bafl_S[1][2]) -
		 bafl_S[1][0]*(bafl_S[2][2]*bafl_S[0][1]-bafl_S[2][1]*bafl_S[0][2]) +
		 bafl_S[2][0]*(bafl_S[1][2]*bafl_S[0][1]-bafl_S[1][1]*bafl_S[0][2]);

  if (detS != 0.){	//check needed? always invertible?
	bafl_invS[0][0]= (bafl_S[2][2]*bafl_S[1][1] - bafl_S[2][1]*bafl_S[1][2]) / detS;
	bafl_invS[0][1]= (bafl_S[2][1]*bafl_S[0][2] - bafl_S[2][2]*bafl_S[0][1]) / detS;
	bafl_invS[0][2]= (bafl_S[1][2]*bafl_S[0][1] - bafl_S[1][1]*bafl_S[0][2]) / detS;

	bafl_invS[1][0]= (bafl_S[2][0]*bafl_S[1][2] - bafl_S[2][2]*bafl_S[1][0]) / detS;
	bafl_invS[1][1]= (bafl_S[2][2]*bafl_S[0][0] - bafl_S[2][0]*bafl_S[0][2]) / detS;
	bafl_invS[1][2]= (bafl_S[1][0]*bafl_S[0][2] - bafl_S[1][2]*bafl_S[0][0]) / detS;

	bafl_invS[2][0]= (bafl_S[2][1]*bafl_S[1][0] - bafl_S[2][0]*bafl_S[1][1]) / detS;
	bafl_invS[2][1]= (bafl_S[2][1]*bafl_S[0][0] - bafl_S[2][0]*bafl_S[0][1]) / detS;
	bafl_invS[2][2]= (bafl_S[1][1]*bafl_S[0][0] - bafl_S[1][0]*bafl_S[0][1]) / detS;
  }

  /* temp_K(6x3) = P_prio(6x6) * HT(6x3)
   *
   * last 3 rows of HT are zero
   */
  for (i=0; i<6; i++) {
	for (j=1; j<3; j++) {
	  bafl_tempK[i][j] = bafl_Pprio[i][0] * bafl_H[j][0];		/* H[j][0] = HT[0][j] */
	  for (k=1; k<3; k++) {										/* normally k<6, not computing zero elements */
		bafl_tempK[i][j] +=  bafl_Pprio[i][k] * bafl_H[j][k]; 	/* H[j][k] = HT[k][j] */
	  }
	}
  }

  /* K(6x3) = temp_K(6x3) * invS(3x3)
   */
  for (i=0; i<6; i++) {
	for (j=1; j<3; j++) {
	  bafl_K[i][j] = bafl_tempK[i][0] * bafl_invS[0][j];
	  for (k=1; k<3; k++) {
		bafl_K[i][j] +=  bafl_tempK[i][k] * bafl_invS[k][j];
	  }
	}
  }
  
  
  
  /**********************************************  
   * Update filter state.
   *
   *  The a priori filter state is zero since the errors are corrected after each update.
   * 
   *  X = X_prio + K (y - H * X_prio)
   *  X = K * y
   **********************************************/

  
  /* innovation 
   * y = Cnb * -[0; 0; g] - accel 
   */
  bafl_y.x = -RMAT_ELMT(bafl_dcm, 0, 2) * bafl_g - bafl_accel.x;
  bafl_y.y = -RMAT_ELMT(bafl_dcm, 1, 2) * bafl_g - bafl_accel.y;
  bafl_y.z = -RMAT_ELMT(bafl_dcm, 2, 2) * bafl_g - bafl_accel.z;
  
  /* X(6) = K(6x3) * y(3) 
   */
  for (i=0; i<6; i++) {
	bafl_X[i]  = bafl_K[i][0] * bafl_y.x;
	bafl_X[i] += bafl_K[i][1] * bafl_y.y;
	bafl_X[i] += bafl_K[i][2] * bafl_y.z;
  }
  
  /**********************************************  
   * Update the filter covariance.
   *
   *  P = P_prio - K * H * P_prio
   *  P = ( I - K * H ) * P_prio
   *
   **********************************************/
   
  /*  temp(6x6) = I(6x6) - K(6x3)*H(3x6)
   *  
   *  last 3 columns of H are zero
   */
  for (i=0; i<6; i++) {
	for (j=0; j<6; j++) {
	  if (i==j) {
		bafl_tempP[i][j] = 1.;
	  } 
	  else {
		bafl_tempP[i][j] = 0.;
	  }
	  if (j < 3) {				/* omit the parts where H is zero */
		for (k=0; k<3; k++) {
		  bafl_tempP[i][j] -= bafl_K[i][k] * bafl_H[k][j];
		}
	  }
	}
  }

  /*  P(6x6) = temp(6x6) * P_prio(6x6)
   */
  for (i=0; i<BAFL_SSIZE; i++) {
	for (j=0; j<BAFL_SSIZE; j++) {
	  bafl_P[i][j] = bafl_tempP[i][0] * bafl_Pprio[0][j];
	  for (k=1; k<BAFL_SSIZE; k++) {
		bafl_P[i][j] += bafl_tempP[i][k] * bafl_Pprio[k][j];
	  }
	}
  }
  
  
  
  /**********************************************
   *  Correct errors.
   *
   *
   **********************************************/
   
  /*  Error quaternion.
   */
  float q_sq;
  q_sq = (bafl_X[0]*bafl_X[0] + bafl_X[1]*bafl_X[1] + bafl_X[1]*bafl_X[1]) / 4;
  if (q_sq > 1) { 			/* this should actually never happen */
	bafl_quat_err.qi = 1;
	bafl_quat_err.qx = bafl_X[0] / 2;
	bafl_quat_err.qy = bafl_X[1] / 2;
	bafl_quat_err.qz = bafl_X[2] / 2;
	FLOAT_QUAT_SMUL(bafl_quat_err, bafl_quat_err, 1/sqrtf(1 + q_sq));
  }
  else {
	bafl_quat_err.qi = sqrtf(1 - q_sq);
	bafl_quat_err.qx = bafl_X[0] / 2;
	bafl_quat_err.qy = bafl_X[1] / 2;
	bafl_quat_err.qz = bafl_X[2] / 2;
  }

  /*  correct attitude
   */
  FLOAT_QUAT_COMP(bafl_quat, bafl_quat, bafl_quat_err);

  /*  correct gyro bias
   */
  RATES_ASSIGN(bafl_bias_err, bafl_X[3], bafl_X[4], bafl_X[5]);
  RATES_SUB(bafl_bias, bafl_bias_err);
  

  /* maintain rotation matrix representation */
  FLOAT_RMAT_OF_QUAT(bafl_dcm, bafl_quat);
   
}




void booz_ahrs_update_mag(void) {
  int i, j, k;
  
  
  MAGS_FLOAT_OF_BFP(bafl_mag, booz_imu.mag);
  
  /* P_prio = P */
  for ( i=0; i<BAFL_SSIZE; i++ ) {
	for ( j=0; j<BAFL_SSIZE; j++ ) {
	  bafl_Pprio[i][j] = bafl_P[i][j];
	}
  }
  
  /* set up measurement matrix */ 
  bafl_H[0][0] = -RMAT_ELMT(bafl_dcm, 0, 1) * bafl_g;
  bafl_H[0][1] =  RMAT_ELMT(bafl_dcm, 0, 0) * bafl_g;
  bafl_H[1][0] = -RMAT_ELMT(bafl_dcm, 1, 1) * bafl_g;
  bafl_H[1][1] =  RMAT_ELMT(bafl_dcm, 1, 0) * bafl_g;
  bafl_H[2][0] = -RMAT_ELMT(bafl_dcm, 2, 1) * bafl_g;
  bafl_H[2][1] =  RMAT_ELMT(bafl_dcm, 2, 0) * bafl_g;
  //rest is zero
  
  
  
  
  
  /**********************************************
   * compute Kalman gain K
   * 
   * K = P_prio * H_T * inv(S)
   * S = H * P_prio * HT + R
   *
   * K = P_prio * HT * inv(H * P_prio * HT + R)
   *
   **********************************************/
  
  
  /* covariance residual S = H * P_prio * HT + R    */
  
  /* temp_S(3x6) = H(3x6) * P_prio(6x6)
   * last 3 columns of H are zero
   */
  for (i=0; i<3; i++) {
	for (j=0; j<6; j++) {
	  bafl_tempS[i][j] = bafl_H[i][0] * bafl_Pprio[0][j];
	  for (k=1; k<3; k++) {
		bafl_tempS[i][j] +=  bafl_H[i][k] * bafl_Pprio[k][j];
	  }
	}
  }

  
  /* S(3x3) = temp_S(3x6) * HT(6x3) + R(3x3)
   *
   * last 3 rows of HT are zero
   */
  for (i=0; i<3; i++) {
	for (j=0; j<3; j++) {
	  if (i < 3 && i == j) {
		bafl_S[i][j] = BAFL_R_MAG;
	  } else {
		bafl_S[i][j] = 0.;
	  }
	  for (k=0; k<3; k++) {		/* normally k<6, not computing zero elements */
		bafl_S[i][j] +=  bafl_tempS[i][k] * bafl_H[j][k]; /* H[j][k] = HT[k][j] */
	  }
	}
  }
  
  /* invert S 
   */
  float detS = 0.;
  detS = bafl_S[0][0]*(bafl_S[2][2]*bafl_S[1][1]-bafl_S[2][1]*bafl_S[1][2]) -
		 bafl_S[1][0]*(bafl_S[2][2]*bafl_S[0][1]-bafl_S[2][1]*bafl_S[0][2]) +
		 bafl_S[2][0]*(bafl_S[1][2]*bafl_S[0][1]-bafl_S[1][1]*bafl_S[0][2]);

  if (detS != 0.){	//check needed? always invertible?
	bafl_invS[0][0]= (bafl_S[2][2]*bafl_S[1][1] - bafl_S[2][1]*bafl_S[1][2]) / detS;
	bafl_invS[0][1]= (bafl_S[2][1]*bafl_S[0][2] - bafl_S[2][2]*bafl_S[0][1]) / detS;
	bafl_invS[0][2]= (bafl_S[1][2]*bafl_S[0][1] - bafl_S[1][1]*bafl_S[0][2]) / detS;

	bafl_invS[1][0]= (bafl_S[2][0]*bafl_S[1][2] - bafl_S[2][2]*bafl_S[1][0]) / detS;
	bafl_invS[1][1]= (bafl_S[2][2]*bafl_S[0][0] - bafl_S[2][0]*bafl_S[0][2]) / detS;
	bafl_invS[1][2]= (bafl_S[1][0]*bafl_S[0][2] - bafl_S[1][2]*bafl_S[0][0]) / detS;

	bafl_invS[2][0]= (bafl_S[2][1]*bafl_S[1][0] - bafl_S[2][0]*bafl_S[1][1]) / detS;
	bafl_invS[2][1]= (bafl_S[2][1]*bafl_S[0][0] - bafl_S[2][0]*bafl_S[0][1]) / detS;
	bafl_invS[2][2]= (bafl_S[1][1]*bafl_S[0][0] - bafl_S[1][0]*bafl_S[0][1]) / detS;
  }

  /* temp_K(6x3) = P_prio(6x6) * HT(6x3)
   *
   * last 3 rows of HT are zero
   */
  for (i=0; i<6; i++) {
	for (j=1; j<3; j++) {
	  bafl_tempK[i][j] = bafl_Pprio[i][0] * bafl_H[j][0];		/* H[j][0] = HT[0][j] */
	  for (k=1; k<3; k++) {										/* normally k<6, not computing zero elements */
		bafl_tempK[i][j] +=  bafl_Pprio[i][k] * bafl_H[j][k]; 	/* H[j][k] = HT[k][j] */
	  }
	}
  }

  /* K(6x3) = temp_K(6x3) * invS(3x3)
   */
  for (i=0; i<6; i++) {
	for (j=1; j<3; j++) {
	  bafl_K[i][j] = bafl_tempK[i][0] * bafl_invS[0][j];
	  for (k=1; k<3; k++) {
		bafl_K[i][j] +=  bafl_tempK[i][k] * bafl_invS[k][j];
	  }
	}
  }
  
  
  
  /**********************************************  
   * Update filter state.
   *
   *  The a priori filter state is zero since the errors are corrected after each update.
   * 
   *  X = X_prio + K (y - H * X_prio)
   *  X = K * y
   **********************************************/

  
  /* innovation 
   * y = Cnb * -[0; 0; g] - accel 
   */
  bafl_y.x = -RMAT_ELMT(bafl_dcm, 0, 2) * bafl_g - bafl_accel.x;
  bafl_y.y = -RMAT_ELMT(bafl_dcm, 1, 2) * bafl_g - bafl_accel.y;
  bafl_y.z = -RMAT_ELMT(bafl_dcm, 2, 2) * bafl_g - bafl_accel.z;
  
  /* X(6) = K(6x3) * y(3) 
   */
  for (i=0; i<6; i++) {
	bafl_X[i]  = bafl_K[i][0] * bafl_y.x;
	bafl_X[i] += bafl_K[i][1] * bafl_y.y;
	bafl_X[i] += bafl_K[i][2] * bafl_y.z;
  }
  
  /**********************************************  
   * Update the filter covariance.
   *
   *  P = P_prio - K * H * P_prio
   *  P = ( I - K * H ) * P_prio
   *
   **********************************************/
   
  /*  temp(6x6) = I(6x6) - K(6x3)*H(3x6)
   *  
   *  last 3 columns of H are zero
   */
  for (i=0; i<6; i++) {
	for (j=0; j<6; j++) {
	  if (i==j) {
		bafl_tempP[i][j] = 1.;
	  } 
	  else {
		bafl_tempP[i][j] = 0.;
	  }
	  if (j < 3) {				/* omit the parts where H is zero */
		for (k=0; k<3; k++) {
		  bafl_tempP[i][j] -= bafl_K[i][k] * bafl_H[k][j];
		}
	  }
	}
  }

  /*  P(6x6) = temp(6x6) * P_prio(6x6)
   */
  for (i=0; i<BAFL_SSIZE; i++) {
	for (j=0; j<BAFL_SSIZE; j++) {
	  bafl_P[i][j] = bafl_tempP[i][0] * bafl_Pprio[0][j];
	  for (k=1; k<BAFL_SSIZE; k++) {
		bafl_P[i][j] += bafl_tempP[i][k] * bafl_Pprio[k][j];
	  }
	}
  }
  
  
  
  /**********************************************
   *  Correct errors.
   *
   *
   **********************************************/
   
  /*  Error quaternion.
   */
  float q_sq;
  q_sq = (bafl_X[0]*bafl_X[0] + bafl_X[1]*bafl_X[1] + bafl_X[1]*bafl_X[1]) / 4;
  if (q_sq > 1) { 			/* this should actually never happen */
	bafl_quat_err.qi = 1;
	bafl_quat_err.qx = bafl_X[0] / 2;
	bafl_quat_err.qy = bafl_X[1] / 2;
	bafl_quat_err.qz = bafl_X[2] / 2;
	FLOAT_QUAT_SMUL(bafl_quat_err, bafl_quat_err, 1/sqrtf(1 + q_sq));
  }
  else {
	bafl_quat_err.qi = sqrtf(1 - q_sq);
	bafl_quat_err.qx = bafl_X[0] / 2;
	bafl_quat_err.qy = bafl_X[1] / 2;
	bafl_quat_err.qz = bafl_X[2] / 2;
  }

  /*  correct attitude
   */
  FLOAT_QUAT_COMP(bafl_quat, bafl_quat, bafl_quat_err);

  /*  correct gyro bias
   */
  RATES_ASSIGN(bafl_bias_err, bafl_X[3], bafl_X[4], bafl_X[5]);
  RATES_SUB(bafl_bias, bafl_bias_err);
  

  /* maintain rotation matrix representation */
  FLOAT_RMAT_OF_QUAT(bafl_dcm, bafl_quat);
}