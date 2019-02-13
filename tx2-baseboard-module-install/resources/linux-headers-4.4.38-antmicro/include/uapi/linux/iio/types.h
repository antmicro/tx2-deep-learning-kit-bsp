/* industrial I/O data types needed both in and out of kernel
 *
 * Copyright (c) 2008 Jonathan Cameron
 * Copyright (c) 2014-2016, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#ifndef _UAPI_IIO_TYPES_H_
#define _UAPI_IIO_TYPES_H_

enum iio_chan_type {
	IIO_VOLTAGE,
	IIO_CURRENT,
	IIO_POWER,
	IIO_ACCEL,
	IIO_ANGL_VEL,
	IIO_MAGN,
	IIO_LIGHT,
	IIO_INTENSITY,
	IIO_PROXIMITY,
	IIO_TEMP,
	IIO_INCLI,
	IIO_ROT,
	IIO_ANGL,
	IIO_TIMESTAMP,
	IIO_CAPACITANCE,
	IIO_ALTVOLTAGE,
	IIO_CCT,
	IIO_PRESSURE,
	IIO_HUMIDITYRELATIVE,
	IIO_ACTIVITY,
	IIO_STEPS,
	IIO_ENERGY,
	IIO_DISTANCE,
	IIO_VELOCITY,
	IIO_CONCENTRATION,
	IIO_RESISTANCE,
	IIO_ORIENTATION,
	IIO_GRAVITY,
	IIO_LINEAR_ACCEL,
	IIO_HUMIDITY,
	IIO_GAME_ROT,
	IIO_MOTION,
	IIO_STEP,
	IIO_STEP_COUNT,
	IIO_GEOMAGN_ROT,
	IIO_HEART_RATE,
	IIO_GESTURE_WAKE,
	IIO_GESTURE_GLANCE,
	IIO_GESTURE_PICKUP,
	IIO_GESTURE_WRIST_TILT,
	IIO_DEVICE_ORIENTATION,
	IIO_POSE_6DOF,
	IIO_STATIONARY_DETECT,
	IIO_MOTION_DETECT,
	IIO_HEART_BEAT,
	IIO_DYNAMIC_SENSOR_META,
	IIO_ADDITIONAL_INFO,
	IIO_GENERIC,
};


enum iio_modifier {
	IIO_NO_MOD,
	IIO_MOD_X,
	IIO_MOD_Y,
	IIO_MOD_Z,
	IIO_MOD_W,
	IIO_MOD_COS,
	IIO_MOD_X_AND_Y,
	IIO_MOD_X_AND_Z,
	IIO_MOD_Y_AND_Z,
	IIO_MOD_X_AND_Y_AND_Z,
	IIO_MOD_X_OR_Y,
	IIO_MOD_X_OR_Z,
	IIO_MOD_Y_OR_Z,
	IIO_MOD_X_OR_Y_OR_Z,
	IIO_MOD_LIGHT_BOTH,
	IIO_MOD_LIGHT_IR,
	IIO_MOD_ROOT_SUM_SQUARED_X_Y,
	IIO_MOD_SUM_SQUARED_X_Y_Z,
	IIO_MOD_LIGHT_CLEAR,
	IIO_MOD_LIGHT_RED,
	IIO_MOD_LIGHT_GREEN,
	IIO_MOD_LIGHT_BLUE,
	IIO_MOD_QUATERNION,
	IIO_MOD_TEMP_AMBIENT,
	IIO_MOD_TEMP_OBJECT,
	IIO_MOD_NORTH_MAGN,
	IIO_MOD_NORTH_TRUE,
	IIO_MOD_NORTH_MAGN_TILT_COMP,
	IIO_MOD_NORTH_TRUE_TILT_COMP,
	IIO_MOD_RUNNING,
	IIO_MOD_JOGGING,
	IIO_MOD_WALKING,
	IIO_MOD_STILL,
	IIO_MOD_ROOT_SUM_SQUARED_X_Y_Z,
	IIO_MOD_I,
	IIO_MOD_Q,
	IIO_MOD_CO2,
	IIO_MOD_VOC,
	IIO_MOD_X_UNCALIB,
	IIO_MOD_Y_UNCALIB,
	IIO_MOD_Z_UNCALIB,
	IIO_MOD_X_BIAS,
	IIO_MOD_Y_BIAS,
	IIO_MOD_Z_BIAS,
	IIO_MOD_STATUS,
	IIO_MOD_BPM,
};

enum iio_event_type {
	IIO_EV_TYPE_THRESH,
	IIO_EV_TYPE_MAG,
	IIO_EV_TYPE_ROC,
	IIO_EV_TYPE_THRESH_ADAPTIVE,
	IIO_EV_TYPE_MAG_ADAPTIVE,
	IIO_EV_TYPE_CHANGE,
};

enum iio_event_direction {
	IIO_EV_DIR_EITHER,
	IIO_EV_DIR_RISING,
	IIO_EV_DIR_FALLING,
	IIO_EV_DIR_NONE,
};

#endif /* _UAPI_IIO_TYPES_H_ */

