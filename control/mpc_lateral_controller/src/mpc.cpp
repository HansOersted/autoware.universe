// Copyright 2018-2021 The Autoware Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mpc_lateral_controller/mpc.hpp"

#include "interpolation/linear_interpolation.hpp"
#include "motion_utils/trajectory/trajectory.hpp"
#include "mpc_lateral_controller/mpc_utils.hpp"
#include "rclcpp/rclcpp.hpp"
#include "tier4_autoware_utils/math/unit_conversion.hpp"

#include <algorithm>
#include <limits>

namespace autoware::motion::control::mpc_lateral_controller
{
using tier4_autoware_utils::calcDistance2d;
using tier4_autoware_utils::normalizeRadian;
using tier4_autoware_utils::rad2deg;

MPC::MPC(rclcpp::Node & node)
{
  m_debug_frenet_predicted_trajectory_pub = node.create_publisher<Trajectory>(
    "~/debug/predicted_trajectory_in_frenet_coordinate", rclcpp::QoS(1));
}

bool MPC::calculateMPC(
  const SteeringReport & current_steer, const Odometry & current_kinematics,
  AckermannLateralCommand & ctrl_cmd, Trajectory & predicted_trajectory,
  Float32MultiArrayStamped & diagnostic)
{
  // since the reference trajectory does not take into account the current velocity of the ego
  // vehicle, it needs to calculate the trajectory velocity considering the longitudinal dynamics.
  const auto reference_trajectory =
    applyVelocityDynamicsFilter(m_reference_trajectory, current_kinematics);

  // get the necessary data
  const auto [success_data, mpc_data] =
    getData(reference_trajectory, current_steer, current_kinematics);
  if (!success_data) {
    return fail_warn_throttle("fail to get MPC Data. Stop MPC.");
  }

  // calculate initial state of the error dynamics
  const auto x0 = getInitialState(mpc_data);

  // apply time delay compensation to the initial state
  const auto [success_delay, x0_delayed] =
    updateStateForDelayCompensation(reference_trajectory, mpc_data.nearest_time, x0);
  if (!success_delay) {
    return fail_warn_throttle("delay compensation failed. Stop MPC.");
  }

  // resample reference trajectory with mpc sampling time
  const double mpc_start_time = mpc_data.nearest_time + m_param.input_delay;
  const double prediction_dt =
    getPredictionDeltaTime(mpc_start_time, reference_trajectory, current_kinematics);

  const auto [success_resample, mpc_resampled_ref_trajectory] =
    resampleMPCTrajectoryByTime(mpc_start_time, prediction_dt, reference_trajectory);
  if (!success_resample) {
    return fail_warn_throttle("trajectory resampling failed. Stop MPC.");
  }

  // get the diagnostic data
  const auto [success_data_for_diagnostic, mpc_data_for_diagnostic] =
    getData(mpc_resampled_ref_trajectory, current_steer, current_kinematics);
  if (!success_data_for_diagnostic) {
    return fail_warn_throttle("fail to get MPC Data for the diagnostic. Stop MPC.");
  }

  // get the diagnostic data w.r.t. the original trajectory
  const auto [success_data_traj_raw, mpc_data_traj_raw] =
    getData(mpc_traj_raw, current_steer, current_kinematics);
  if (!success_data_traj_raw) {
    return fail_warn_throttle("fail to get MPC Data for the raw trajectory. Stop MPC.");
  }

  // generate mpc matrix : predict equation Xec = Aex * x0 + Bex * Uex + Wex
  const auto mpc_matrix = generateMPCMatrix(mpc_resampled_ref_trajectory, prediction_dt);

  // solve Optimization problem
  const auto [success_opt, Uex] = executeOptimization(
    mpc_matrix, x0_delayed, prediction_dt, mpc_resampled_ref_trajectory,
    current_kinematics.twist.twist.linear.x);
  if (!success_opt) {
    return fail_warn_throttle("optimization failed. Stop MPC.");
  }

  // apply filters for the input limitation and low pass filter
  const double u_saturated = std::clamp(Uex(0), -m_steer_lim, m_steer_lim);
  const double u_filtered = m_lpf_steering_cmd.filter(u_saturated);

  // set control command
  ctrl_cmd.steering_tire_angle = static_cast<float>(u_filtered);
  ctrl_cmd.steering_tire_rotation_rate = static_cast<float>(calcDesiredSteeringRate(
    mpc_matrix, x0_delayed, Uex, u_filtered, current_steer.steering_tire_angle, prediction_dt));

  // save the control command for the steering prediction
  m_steering_predictor->storeSteerCmd(u_filtered);

  // save input to buffer for delay compensation
  m_input_buffer.push_back(ctrl_cmd.steering_tire_angle);
  m_input_buffer.pop_front();

  // save previous input for the mpc rate limit
  m_raw_steer_cmd_pprev = m_raw_steer_cmd_prev;
  m_raw_steer_cmd_prev = Uex(0);

  /* calculate predicted trajectory */
  predicted_trajectory =
    calculatePredictedTrajectory(mpc_matrix, x0, Uex, mpc_resampled_ref_trajectory, prediction_dt);

  // prepare diagnostic message
  diagnostic = generateDiagData(
    mpc_resampled_ref_trajectory, mpc_data_traj_raw, mpc_data_for_diagnostic, mpc_matrix, ctrl_cmd,
    Uex, current_kinematics);

  return true;
}

Float32MultiArrayStamped MPC::generateDiagData(
  const MPCTrajectory & reference_trajectory, const MPCData & mpc_data_traj_raw,
  const MPCData & mpc_data, const MPCMatrix & mpc_matrix, const AckermannLateralCommand & ctrl_cmd,
  const VectorXd & Uex, const Odometry & current_kinematics) const
{
  Float32MultiArrayStamped diagnostic;

  // prepare diagnostic message
  const double nearest_k = reference_trajectory.k.at(mpc_data.nearest_idx);
  const double nearest_smooth_k = reference_trajectory.smooth_k.at(mpc_data.nearest_idx);
  const double wb = m_vehicle_model_ptr->getWheelbase();
  const double current_velocity = current_kinematics.twist.twist.linear.x;
  const double wz_predicted = current_velocity * std::tan(mpc_data.predicted_steer) / wb;
  const double wz_measured = current_velocity * std::tan(mpc_data.steer) / wb;
  const double wz_command = current_velocity * std::tan(ctrl_cmd.steering_tire_angle) / wb;
  const int iteration_num = m_qpsolver_ptr->getTakenIter();
  const double runtime = m_qpsolver_ptr->getRunTime();
  const double objective_value = m_qpsolver_ptr->getObjVal();

  typedef decltype(diagnostic.data)::value_type DiagnosticValueType;
  const auto append_diag = [&](const auto & val) -> void {
    diagnostic.data.push_back(static_cast<DiagnosticValueType>(val));
  };
  append_diag(ctrl_cmd.steering_tire_angle);      // [0] final steering command (MPC + LPF)
  append_diag(Uex(0));                            // [1] mpc calculation result
  append_diag(mpc_matrix.Uref_ex(0));             // [2] feed-forward steering value
  append_diag(std::atan(nearest_smooth_k * wb));  // [3] feed-forward steering value raw
  append_diag(mpc_data.steer);                    // [4] current steering angle
  append_diag(mpc_data.lateral_err);              // [5] lateral error
  append_diag(tf2::getYaw(current_kinematics.pose.pose.orientation));  // [6] current_pose yaw
  append_diag(tf2::getYaw(mpc_data.nearest_pose.orientation));         // [7] nearest_pose yaw
  append_diag(mpc_data.yaw_err);                                       // [8] yaw error
  append_diag(reference_trajectory.vx.at(mpc_data.nearest_idx));       // [9] reference velocity
  append_diag(current_velocity);                                       // [10] measured velocity
  append_diag(wz_command);                           // [11] angular velocity from steer command
  append_diag(wz_measured);                          // [12] angular velocity from measured steer
  append_diag(current_velocity * nearest_smooth_k);  // [13] angular velocity from path curvature
  append_diag(nearest_smooth_k);          // [14] nearest path curvature (used for feed-forward)
  append_diag(nearest_k);                 // [15] nearest path curvature (not smoothed)
  append_diag(mpc_data.predicted_steer);  // [16] predicted steer
  append_diag(wz_predicted);              // [17] angular velocity from predicted steer
  append_diag(iteration_num);             // [18] iteration number
  append_diag(runtime);                   // [19] runtime of the latest problem solved
  append_diag(objective_value);           // [20] objective value of the latest problem solved
  append_diag(std::clamp(
    Uex(0), -m_steer_lim,
    m_steer_lim));  // [21] control signal after the saturation constraint (clamp)
  append_diag(mpc_data_traj_raw.lateral_err);  // [22] lateral error from raw trajectory

  return diagnostic;
}

void MPC::setReferenceTrajectory(
  const Trajectory & trajectory_msg, const TrajectoryFilteringParam & param,
  const Odometry & current_kinematics)
{
  const size_t nearest_seg_idx = motion_utils::findFirstNearestSegmentIndexWithSoftConstraints(
    trajectory_msg.points, current_kinematics.pose.pose, ego_nearest_dist_threshold,
    ego_nearest_yaw_threshold);
  const double ego_offset_to_segment = motion_utils::calcLongitudinalOffsetToSegment(
    trajectory_msg.points, nearest_seg_idx, current_kinematics.pose.pose.position);

  mpc_traj_raw = MPCUtils::convertToMPCTrajectory(trajectory_msg);

  // resampling
  const auto [success_resample, mpc_traj_resampled] = MPCUtils::resampleMPCTrajectoryByDistance(
    mpc_traj_raw, param.traj_resample_dist, nearest_seg_idx, ego_offset_to_segment);
  if (!success_resample) {
    warn_throttle("[setReferenceTrajectory] spline error when resampling by distance");
    return;
  }

  const auto is_forward_shift =
    motion_utils::isDrivingForward(mpc_traj_resampled.toTrajectoryPoints());

  // if driving direction is unknown, use previous value
  m_is_forward_shift = is_forward_shift ? is_forward_shift.value() : m_is_forward_shift;

  // path smoothing
  MPCTrajectory mpc_traj_smoothed = mpc_traj_resampled;  // smooth filtered trajectory
  const int mpc_traj_resampled_size = static_cast<int>(mpc_traj_resampled.size());
  if (
    param.enable_path_smoothing && mpc_traj_resampled_size > 2 * param.path_filter_moving_ave_num) {
    using MoveAverageFilter::filt_vector;
    if (
      !filt_vector(param.path_filter_moving_ave_num, mpc_traj_smoothed.x) ||
      !filt_vector(param.path_filter_moving_ave_num, mpc_traj_smoothed.y) ||
      !filt_vector(param.path_filter_moving_ave_num, mpc_traj_smoothed.yaw) ||
      !filt_vector(param.path_filter_moving_ave_num, mpc_traj_smoothed.vx)) {
      RCLCPP_DEBUG(m_logger, "path callback: filtering error. stop filtering.");
      mpc_traj_smoothed = mpc_traj_resampled;
    }
  }

  /*
   * Extend terminal points
   * Note: The current MPC does not properly take into account the attitude angle at the end of the
   * path. By extending the end of the path in the attitude direction, the MPC can consider the
   * attitude angle well, resulting in improved control performance. If the trajectory is
   * well-defined considering the end point attitude angle, this feature is not necessary.
   */
  if (param.extend_trajectory_for_end_yaw_control) {
    MPCUtils::extendTrajectoryInYawDirection(
      mpc_traj_raw.yaw.back(), param.traj_resample_dist, m_is_forward_shift, mpc_traj_smoothed);
  }

  // calculate yaw angle
  MPCUtils::calcTrajectoryYawFromXY(mpc_traj_smoothed, m_is_forward_shift);
  MPCUtils::convertEulerAngleToMonotonic(mpc_traj_smoothed.yaw);

  // calculate curvature
  MPCUtils::calcTrajectoryCurvature(
    param.curvature_smoothing_num_traj, param.curvature_smoothing_num_ref_steer, mpc_traj_smoothed);

  // stop velocity at a terminal point
  mpc_traj_smoothed.vx.back() = 0.0;

  // add a extra point on back with extended time to make the mpc stable.
  auto last_point = mpc_traj_smoothed.back();
  last_point.relative_time += 100.0;  // extra time to prevent mpc calc failure due to short time
  last_point.vx = 0.0;                // stop velocity at a terminal point
  mpc_traj_smoothed.push_back(last_point);

  if (!mpc_traj_smoothed.size()) {
    RCLCPP_DEBUG(m_logger, "path callback: trajectory size is undesired.");
    return;
  }

  m_reference_trajectory = mpc_traj_smoothed;
}

void MPC::resetPrevResult(const SteeringReport & current_steer)
{
  // Consider limit. The prev value larger than limitation brakes the optimization constraint and
  // results in optimization failure.
  const float steer_lim_f = static_cast<float>(m_steer_lim);
  m_raw_steer_cmd_prev = std::clamp(current_steer.steering_tire_angle, -steer_lim_f, steer_lim_f);
  m_raw_steer_cmd_pprev = std::clamp(current_steer.steering_tire_angle, -steer_lim_f, steer_lim_f);
}

std::pair<bool, MPCData> MPC::getData(
  const MPCTrajectory & traj, const SteeringReport & current_steer,
  const Odometry & current_kinematics)
{
  const auto current_pose = current_kinematics.pose.pose;

  MPCData data;
  if (!MPCUtils::calcNearestPoseInterp(
        traj, current_pose, &(data.nearest_pose), &(data.nearest_idx), &(data.nearest_time),
        ego_nearest_dist_threshold, ego_nearest_yaw_threshold)) {
    warn_throttle("calculateMPC: error in calculating nearest pose. stop mpc.");
    return {false, MPCData{}};
  }

  // get data
  data.steer = static_cast<double>(current_steer.steering_tire_angle);
  data.lateral_err = MPCUtils::calcLateralError(current_pose, data.nearest_pose);
  data.yaw_err = normalizeRadian(
    tf2::getYaw(current_pose.orientation) - tf2::getYaw(data.nearest_pose.orientation));

  // get predicted steer
  data.predicted_steer = m_steering_predictor->calcSteerPrediction();

  // check error limit
  const double dist_err = calcDistance2d(current_pose, data.nearest_pose);
  if (dist_err > m_admissible_position_error) {
    warn_throttle("Too large position error: %fm > %fm", dist_err, m_admissible_position_error);
    return {false, MPCData{}};
  }

  // check yaw error limit
  if (std::fabs(data.yaw_err) > m_admissible_yaw_error_rad) {
    warn_throttle("Too large yaw error: %f > %f", data.yaw_err, m_admissible_yaw_error_rad);
    return {false, MPCData{}};
  }

  // check trajectory time length
  const double max_prediction_time =
    m_param.min_prediction_length / static_cast<double>(m_param.prediction_horizon - 1);
  auto end_time = data.nearest_time + m_param.input_delay + m_ctrl_period + max_prediction_time;
  if (end_time > traj.relative_time.back()) {
    warn_throttle("path is too short for prediction.");
    return {false, MPCData{}};
  }
  return {true, data};
}

std::pair<bool, MPCTrajectory> MPC::resampleMPCTrajectoryByTime(
  const double ts, const double prediction_dt, const MPCTrajectory & input) const
{
  MPCTrajectory output;
  std::vector<double> mpc_time_v;
  for (double i = 0; i < static_cast<double>(m_param.prediction_horizon); ++i) {
    mpc_time_v.push_back(ts + i * prediction_dt);
  }
  if (!MPCUtils::linearInterpMPCTrajectory(input.relative_time, input, mpc_time_v, output)) {
    warn_throttle("calculateMPC: mpc resample error. stop mpc calculation. check code!");
    return {false, {}};
  }
  return {true, output};
}

VectorXd MPC::getInitialState(const MPCData & data)
{
  const int DIM_X = m_vehicle_model_ptr->getDimX();
  VectorXd x0 = VectorXd::Zero(DIM_X);

  const auto & lat_err = data.lateral_err;
  const auto & steer = m_use_steer_prediction ? data.predicted_steer : data.steer;
  const auto & yaw_err = data.yaw_err;

  const auto vehicle_model = m_vehicle_model_ptr->modelName();
  if (vehicle_model == "kinematics") {
    x0 << lat_err, yaw_err, steer;
  } else if (vehicle_model == "kinematics_no_delay") {
    x0 << lat_err, yaw_err;
  } else if (vehicle_model == "dynamics") {
    double dlat = (lat_err - m_lateral_error_prev) / m_ctrl_period;
    double dyaw = (yaw_err - m_yaw_error_prev) / m_ctrl_period;
    m_lateral_error_prev = lat_err;
    m_yaw_error_prev = yaw_err;
    dlat = m_lpf_lateral_error.filter(dlat);
    dyaw = m_lpf_yaw_error.filter(dyaw);
    x0 << lat_err, dlat, yaw_err, dyaw;
    RCLCPP_DEBUG(m_logger, "(before lpf) dot_lat_err = %f, dot_yaw_err = %f", dlat, dyaw);
    RCLCPP_DEBUG(m_logger, "(after lpf) dot_lat_err = %f, dot_yaw_err = %f", dlat, dyaw);
  } else {
    RCLCPP_ERROR(m_logger, "vehicle_model_type is undefined");
  }
  return x0;
}

std::pair<bool, VectorXd> MPC::updateStateForDelayCompensation(
  const MPCTrajectory & traj, const double & start_time, const VectorXd & x0_orig)
{
  const int DIM_X = m_vehicle_model_ptr->getDimX();
  const int DIM_U = m_vehicle_model_ptr->getDimU();
  const int DIM_Y = m_vehicle_model_ptr->getDimY();

  MatrixXd Ad(DIM_X, DIM_X);
  MatrixXd Bd(DIM_X, DIM_U);
  MatrixXd Wd(DIM_X, 1);
  MatrixXd Cd(DIM_Y, DIM_X);

  MatrixXd x_curr = x0_orig;
  double mpc_curr_time = start_time;
  for (size_t i = 0; i < m_input_buffer.size(); ++i) {
    double k, v = 0.0;
    try {
      k = interpolation::lerp(traj.relative_time, traj.k, mpc_curr_time);
      v = interpolation::lerp(traj.relative_time, traj.vx, mpc_curr_time);
    } catch (const std::exception & e) {
      RCLCPP_ERROR(m_logger, "mpc resample failed at delay compensation, stop mpc: %s", e.what());
      return {false, {}};
    }

    // get discrete state matrix A, B, C, W
    m_vehicle_model_ptr->setVelocity(v);
    m_vehicle_model_ptr->setCurvature(k);
    m_vehicle_model_ptr->calculateDiscreteMatrix(Ad, Bd, Cd, Wd, m_ctrl_period);
    MatrixXd ud = MatrixXd::Zero(DIM_U, 1);
    ud(0, 0) = m_input_buffer.at(i);  // for steering input delay
    x_curr = Ad * x_curr + Bd * ud + Wd;
    mpc_curr_time += m_ctrl_period;
  }
  return {true, x_curr};
}

MPCTrajectory MPC::applyVelocityDynamicsFilter(
  const MPCTrajectory & input, const Odometry & current_kinematics) const
{
  const auto autoware_traj = MPCUtils::convertToAutowareTrajectory(input);
  if (autoware_traj.points.empty()) {
    return input;
  }

  const size_t nearest_seg_idx = motion_utils::findFirstNearestSegmentIndexWithSoftConstraints(
    autoware_traj.points, current_kinematics.pose.pose, ego_nearest_dist_threshold,
    ego_nearest_yaw_threshold);

  MPCTrajectory output = input;
  MPCUtils::dynamicSmoothingVelocity(
    nearest_seg_idx, current_kinematics.twist.twist.linear.x, m_param.acceleration_limit,
    m_param.velocity_time_constant, output);

  auto last_point = output.back();
  last_point.relative_time += 100.0;  // extra time to prevent mpc calc failure due to short time
  last_point.vx = 0.0;                // stop velocity at a terminal point
  output.push_back(last_point);
  return output;
}

/*
 * predict equation: Xec = Aex * x0 + Bex * Uex + Wex
 * cost function: J = Xex' * Qex * Xex + (Uex - Uref)' * R1ex * (Uex - Uref_ex) + Uex' * R2ex * Uex
 * Qex = diag([Q,Q,...]), R1ex = diag([R,R,...])
 */
MPCMatrix MPC::generateMPCMatrix(
  const MPCTrajectory & reference_trajectory, const double prediction_dt)
{
  const int N = m_param.prediction_horizon;
  const double DT = prediction_dt;
  const int DIM_X = m_vehicle_model_ptr->getDimX();
  const int DIM_U = m_vehicle_model_ptr->getDimU();
  const int DIM_Y = m_vehicle_model_ptr->getDimY();

  MPCMatrix m;
  m.Aex = MatrixXd::Zero(DIM_X * N, DIM_X);
  m.Bex = MatrixXd::Zero(DIM_X * N, DIM_U * N);
  m.Wex = MatrixXd::Zero(DIM_X * N, 1);
  m.Cex = MatrixXd::Zero(DIM_Y * N, DIM_X * N);
  m.Qex = MatrixXd::Zero(DIM_Y * N, DIM_Y * N);
  m.R1ex = MatrixXd::Zero(DIM_U * N, DIM_U * N);
  m.R2ex = MatrixXd::Zero(DIM_U * N, DIM_U * N);
  m.Uref_ex = MatrixXd::Zero(DIM_U * N, 1);

  // weight matrix depends on the vehicle model
  MatrixXd Q = MatrixXd::Zero(DIM_Y, DIM_Y);
  MatrixXd R = MatrixXd::Zero(DIM_U, DIM_U);
  MatrixXd Q_adaptive = MatrixXd::Zero(DIM_Y, DIM_Y);
  MatrixXd R_adaptive = MatrixXd::Zero(DIM_U, DIM_U);

  MatrixXd Ad(DIM_X, DIM_X);
  MatrixXd Bd(DIM_X, DIM_U);
  MatrixXd Wd(DIM_X, 1);
  MatrixXd Cd(DIM_Y, DIM_X);
  MatrixXd Uref(DIM_U, 1);

  const double sign_vx = m_is_forward_shift ? 1 : -1;

  // predict dynamics for N times
  for (int i = 0; i < N; ++i) {
    const double ref_vx = reference_trajectory.vx.at(i);
    const double ref_vx_squared = ref_vx * ref_vx;

    const double ref_k = reference_trajectory.k.at(i) * sign_vx;
    const double ref_smooth_k = reference_trajectory.smooth_k.at(i) * sign_vx;

    // get discrete state matrix A, B, C, W
    m_vehicle_model_ptr->setVelocity(ref_vx);
    m_vehicle_model_ptr->setCurvature(ref_k);
    m_vehicle_model_ptr->calculateDiscreteMatrix(Ad, Bd, Cd, Wd, DT);

    Q = MatrixXd::Zero(DIM_Y, DIM_Y);
    R = MatrixXd::Zero(DIM_U, DIM_U);
    const auto mpc_weight = getWeight(ref_k);
    Q(0, 0) = mpc_weight.lat_error;
    Q(1, 1) = mpc_weight.heading_error;
    R(0, 0) = mpc_weight.steering_input;

    Q_adaptive = Q;
    R_adaptive = R;
    if (i == N - 1) {
      Q_adaptive(0, 0) = m_param.nominal_weight.terminal_lat_error;
      Q_adaptive(1, 1) = m_param.nominal_weight.terminal_heading_error;
    }
    Q_adaptive(1, 1) += ref_vx_squared * mpc_weight.heading_error_squared_vel;
    R_adaptive(0, 0) += ref_vx_squared * mpc_weight.steering_input_squared_vel;

    // update mpc matrix
    int idx_x_i = i * DIM_X;
    int idx_x_i_prev = (i - 1) * DIM_X;
    int idx_u_i = i * DIM_U;
    int idx_y_i = i * DIM_Y;
    if (i == 0) {
      m.Aex.block(0, 0, DIM_X, DIM_X) = Ad;
      m.Bex.block(0, 0, DIM_X, DIM_U) = Bd;
      m.Wex.block(0, 0, DIM_X, 1) = Wd;
    } else {
      m.Aex.block(idx_x_i, 0, DIM_X, DIM_X) = Ad * m.Aex.block(idx_x_i_prev, 0, DIM_X, DIM_X);
      for (int j = 0; j < i; ++j) {
        int idx_u_j = j * DIM_U;
        m.Bex.block(idx_x_i, idx_u_j, DIM_X, DIM_U) =
          Ad * m.Bex.block(idx_x_i_prev, idx_u_j, DIM_X, DIM_U);
      }
      m.Wex.block(idx_x_i, 0, DIM_X, 1) = Ad * m.Wex.block(idx_x_i_prev, 0, DIM_X, 1) + Wd;
    }
    m.Bex.block(idx_x_i, idx_u_i, DIM_X, DIM_U) = Bd;
    m.Cex.block(idx_y_i, idx_x_i, DIM_Y, DIM_X) = Cd;
    m.Qex.block(idx_y_i, idx_y_i, DIM_Y, DIM_Y) = Q_adaptive;
    m.R1ex.block(idx_u_i, idx_u_i, DIM_U, DIM_U) = R_adaptive;

    // get reference input (feed-forward)
    m_vehicle_model_ptr->setCurvature(ref_smooth_k);
    m_vehicle_model_ptr->calculateReferenceInput(Uref);
    if (std::fabs(Uref(0, 0)) < tier4_autoware_utils::deg2rad(m_param.zero_ff_steer_deg)) {
      Uref(0, 0) = 0.0;  // ignore curvature noise
    }
    m.Uref_ex.block(i * DIM_U, 0, DIM_U, 1) = Uref;
  }

  // add lateral jerk : weight for (v * {u(i) - u(i-1)} )^2
  for (int i = 0; i < N - 1; ++i) {
    const double ref_vx = reference_trajectory.vx.at(i);
    const double ref_k = reference_trajectory.k.at(i) * sign_vx;
    const double j = ref_vx * ref_vx * getWeight(ref_k).lat_jerk / (DT * DT);
    const Eigen::Matrix2d J = (Eigen::Matrix2d() << j, -j, -j, j).finished();
    m.R2ex.block(i, i, 2, 2) += J;
  }

  addSteerWeightR(prediction_dt, m.R1ex);

  return m;
}

/*
 * solve quadratic optimization.
 * cost function: J = Xex' * Qex * Xex + (Uex - Uref)' * R1ex * (Uex - Uref_ex) + Uex' * R2ex * Uex
 *                , Qex = diag([Q,Q,...]), R1ex = diag([R,R,...])
 * constraint matrix : lb < U < ub, lbA < A*U < ubA
 * current considered constraint
 *  - steering limit
 *  - steering rate limit
 *
 * (1)lb < u < ub && (2)lbA < Au < ubA --> (3)[lb, lbA] < [I, A]u < [ub, ubA]
 * (1)lb < u < ub ...
 * [-u_lim] < [ u0 ] < [u_lim]
 * [-u_lim] < [ u1 ] < [u_lim]
 *              ~~~
 * [-u_lim] < [ uN ] < [u_lim] (*N... DIM_U)
 * (2)lbA < Au < ubA ...
 * [prev_u0 - au_lim*ctp] < [   u0  ] < [prev_u0 + au_lim*ctp] (*ctp ... ctrl_period)
 * [    -au_lim * dt    ] < [u1 - u0] < [     au_lim * dt    ]
 * [    -au_lim * dt    ] < [u2 - u1] < [     au_lim * dt    ]
 *                            ~~~
 * [    -au_lim * dt    ] < [uN-uN-1] < [     au_lim * dt    ] (*N... DIM_U)
 */
std::pair<bool, VectorXd> MPC::executeOptimization(
  const MPCMatrix & m, const VectorXd & x0, const double prediction_dt, const MPCTrajectory & traj,
  const double current_velocity)
{
  VectorXd Uex;

const Eigen::MatrixXd H = (Eigen::MatrixXd(50,50) <<  2.40456, -0.0701004, 0.00835596, 0.00376653, 0.00416329, 0.00476199, 0.00566342, 0.00681013, 0.00800993, 0.00918759,  0.0103419,  0.0114735,   0.012585,  0.0136832,  0.0147798,  0.0158851,  0.0169949,  0.0181167,  0.0192294,  0.0203016,  0.0213104,  0.0222229,  0.0230208,  0.0236764,  0.0241627,  0.0244562,  0.0245311,   0.024381,  0.0240317,   0.023515,  0.0228526,  0.0220592,  0.0211448,  0.0201169,  0.0189824,  0.0177483,   0.016423,  0.0150168,  0.0135421,  0.0120153,  0.0104558,  0.0088874, 0.00733827, 0.00584136, 0.00443384, 0.00315622, 0.00205059, 0.00115742,0.000509977,0.000125592,
-0.0701004,    1.11298, -0.0552805, 0.00874947, 0.00430868, 0.00492837,  0.0058614, 0.00704832, 0.00829024, 0.00950934,  0.0107044,  0.0118759,  0.0130269,   0.014164,  0.0152998,  0.0164446,  0.0175941,  0.0187562,  0.0199091,  0.0210201,  0.0220656,  0.0230115,   0.023839,  0.0245191,   0.025024,  0.0253293,  0.0254083,  0.0252543,  0.0248939,    0.02436,  0.0236752,  0.0228547,  0.0219087,   0.020845,  0.0196706,   0.018393,  0.0170208,  0.0155645,  0.0140371,  0.0124554,  0.0108398, 0.00921461, 0.00760922, 0.00605773, 0.00459867, 0.00327405, 0.00212753, 0.00120111,0.000529376,0.000130406,
0.00835596, -0.0552805,    1.11334, -0.0548076, 0.00938311, 0.00518349, 0.00616493, 0.00741345,  0.0087199,  0.0100024,  0.0112598,  0.0124925,  0.0137037,  0.0149005,  0.0160959,   0.017301,  0.0185112,  0.0197348,  0.0209488,   0.022119,  0.0232203,   0.024217,  0.0250891,  0.0258063,  0.0263393,  0.0266622,  0.0267469,  0.0265864,  0.0262087,  0.0256483,   0.024929,  0.0240666,   0.023072,  0.0219533,   0.020718,  0.0193738,  0.0179298,   0.016397,  0.0147892,  0.0131239,  0.0114226, 0.00971106, 0.00802008, 0.00638561, 0.00484827, 0.00345233, 0.00224383, 0.00126708,0.000558621,0.000137654,
0.00376653, 0.00874947, -0.0548076,    1.11394, -0.0540364,  0.0104231, 0.00662668, 0.00796888, 0.00937343,  0.0107524,  0.0121044,  0.0134301,  0.0147327,  0.0160201,  0.0173061,  0.0186026,  0.0199048,  0.0212216,  0.0225282,  0.0237879,  0.0249737,  0.0260471,  0.0269868,    0.02776,   0.028335,  0.0286842,  0.0287774,  0.0286066,  0.0282022,  0.0276011,  0.0268289,  0.0259027,  0.0248341,  0.0236318,  0.0223038,  0.0208584,  0.0193054,  0.0176565,  0.0159266,  0.0141347,  0.0123036,  0.0104612, 0.00864067, 0.00688068, 0.00522496, 0.00372124, 0.00241914, 0.00136645,0.000602627,0.000148548,
0.00416329, 0.00430868, 0.00938311, -0.0540364,    1.11491, -0.0527481,  0.0121771, 0.00880958,  0.0103626,  0.0118874,  0.0133826,  0.0148488,  0.0162898,  0.0177139,  0.0191368,  0.0205715,  0.0220127,  0.0234702,  0.0249166,  0.0263114,  0.0276246,  0.0288138,  0.0298552,  0.0307126,  0.0313509,  0.0317395,  0.0318448,  0.0316582,  0.0312129,    0.03055,  0.0296976,  0.0286747,  0.0274939,   0.026165,  0.0246967,  0.0230982,  0.0213803,  0.0195561,  0.0176418,  0.0156585,  0.0136316,  0.0115917, 0.00957567, 0.00762634, 0.00579217, 0.00412601,  0.0026829, 0.00151587,0.000668762,0.000164911,
0.00476199, 0.00492837, 0.00518349,  0.0104231, -0.0527481,    1.11657, -0.0505271,  0.0149294,  0.0118548,  0.0135997,  0.0153108,  0.0169889,  0.0186383,  0.0202687,  0.0218979,  0.0235408,  0.0251913,  0.0268608,  0.0285178,  0.0301159,   0.031621,  0.0329843,  0.0341787,  0.0351626,   0.035896,  0.0363435,  0.0364668,  0.0362558,  0.0357487,  0.0349922,  0.0340185,  0.0328494,  0.0314993,  0.0299794,  0.0282995,  0.0264702,  0.0245038,  0.0224152,  0.0202231,  0.0179515,  0.0156295,  0.0132923,   0.010982, 0.00874769, 0.00664496, 0.00473444, 0.00307927, 0.00174033,0.000768069, 0.00018947,
0.00566342,  0.0058614, 0.00616493, 0.00662668,  0.0121771, -0.0505271,     1.1195, -0.0469199,  0.0189527,  0.0161772,  0.0182132,  0.0202103,  0.0221734,  0.0241141,  0.0260536,  0.0280097,  0.0299751,  0.0319633,   0.033937,  0.0358409,  0.0376343,  0.0392594,  0.0406836,  0.0418576,  0.0427335,  0.0432693,  0.0434191,  0.0431711,  0.0425704,  0.0416728,  0.0405164,  0.0391271,   0.037522,  0.0357144,   0.033716,  0.0315393,  0.0291989,  0.0267126,  0.0241026,  0.0213975,  0.0186318,  0.0158475,  0.0130948,  0.0104322, 0.00792588, 0.00564818, 0.00367441, 0.00207729,0.000917103,0.000226316,
0.00681013, 0.00704832, 0.00741345, 0.00796888, 0.00880958,  0.0149294, -0.0469199,    1.15052, -0.0676057,  0.0243082,  0.0219063,  0.0243092,  0.0266716,  0.0290075,  0.0313421,   0.033697,  0.0360635,  0.0384577,  0.0408349,  0.0431284,  0.0452894,  0.0472481,  0.0489655,   0.050382,    0.05144,  0.0520887,  0.0522731,  0.0519786,  0.0512594,  0.0501825,  0.0487941,  0.0471248,  0.0451955,  0.0430219,  0.0406182,  0.0379994,   0.035183,  0.0321903,  0.0290481,  0.0257907,  0.0224598,  0.0191059,  0.0157894,  0.0125808, 0.00955997, 0.00681406, 0.00443396, 0.00250744, 0.00110742,0.000273385,
0.00800993, 0.00829024,  0.0087199, 0.00937343,  0.0103626,  0.0118548,  0.0189527, -0.0676057,    1.23485,  -0.112971,  0.0306246,  0.0286016,  0.0313828,  0.0341332,  0.0368827,  0.0396564,  0.0424442,  0.0452652,  0.0480667,  0.0507701,  0.0533181,  0.0556286,  0.0576553,  0.0593282,  0.0605793,  0.0613489,  0.0615717,  0.0612305,  0.0603891,  0.0591263,   0.057496,  0.0555347,  0.0532665,  0.0507101,  0.0478821,  0.0447999,  0.0414843,  0.0379601,  0.0342589,  0.0304212,  0.0264961,  0.0225428,  0.0186328,  0.0148492,  0.0112861, 0.00804635, 0.00523737, 0.00296285, 0.00130914,0.000323329,
0.00918759, 0.00950934,  0.0100024,  0.0107524,  0.0118874,  0.0135997,  0.0161772,  0.0243082,  -0.112971,    1.36092,  -0.172375,  0.0376714,  0.0360137,  0.0391728,  0.0423313,  0.0455183,  0.0487223,  0.0519649,  0.0551859,  0.0582951,  0.0612265,  0.0638859,  0.0662202,  0.0681487,  0.0695933,   0.070485,  0.0707489,   0.070365,  0.0694062,  0.0679629,   0.066097,  0.0638502,  0.0612501,   0.058318,  0.0550729,  0.0515349,  0.0477274,  0.0436792,  0.0394264,  0.0350155,  0.0305027,  0.0259565,  0.0214587,  0.0171051,   0.013004, 0.00927395, 0.00603857,  0.0034176, 0.00151089,0.000373363,
 0.0103419,  0.0107044,  0.0112598,  0.0121044,  0.0133826,  0.0153108,  0.0182132,  0.0219063,  0.0306246,  -0.172375,    1.51782,  -0.245862,   0.045412,  0.0441221,  0.0476839,  0.0512787,  0.0548935,  0.0585527,  0.0621886,  0.0656994,   0.069011,  0.0720169,  0.0746572,  0.0768408,  0.0784796,  0.0794955,  0.0798038,  0.0793815,  0.0783108,  0.0766931,  0.0745983,  0.0720729,  0.0691483,  0.0658481,  0.0621937,  0.0582074,  0.0539158,  0.0493512,  0.0445541,   0.039577,  0.0344833,  0.0293502,  0.0242702,  0.0193513,  0.0147161,  0.0104987,  0.0068389, 0.00387255,  0.0017131,0.000423607,
 0.0114735,  0.0118759,  0.0124925,  0.0134301,  0.0148488,  0.0169889,  0.0202103,  0.0243092,  0.0286016,  0.0376714,  -0.245862,    1.70552,  -0.333462,  0.0538355,  0.0529438,  0.0569413,  0.0609622,  0.0650338,  0.0690804,  0.0729897,  0.0766788,  0.0800295,  0.0829751,  0.0854143,  0.0872488,  0.0883916,  0.0887481,  0.0882924,  0.0871155,  0.0853299,   0.083013,  0.0802164,  0.0769745,  0.0733137,  0.0692574,  0.0648304,   0.060062,  0.0549879,  0.0496532,  0.0441162,  0.0384472,  0.0327323,  0.0270744,  0.0215938,  0.0164272,  0.0117241, 0.00764089, 0.00432925, 0.00191653,0.000474262,
  0.012585,  0.0130269,  0.0137037,  0.0147327,  0.0162898,  0.0186383,  0.0221734,  0.0266716,  0.0313828,  0.0360137,   0.045412,  -0.333462,      1.924,  -0.435171,  0.0629768,   0.062522,  0.0669457,  0.0714266,  0.0758818,  0.0801876,  0.0842533,  0.0879487,  0.0912004,  0.0938969,  0.0959298,  0.0972032,  0.0976125,   0.097129,   0.095852,   0.093905,  0.0913727,  0.0883116,  0.0847593,  0.0807445,  0.0762927,  0.0714311,  0.0661916,  0.0606134,   0.054746,  0.0486531,  0.0424125,  0.0361186,  0.0298847,  0.0238436,  0.0181459,  0.0129568, 0.00844888, 0.00479027, 0.00212237,0.000525641,
 0.0136832,   0.014164,  0.0149005,  0.0160201,  0.0177139,  0.0202687,  0.0241141,  0.0290075,  0.0341332,  0.0391728,  0.0441221,  0.0538355,  -0.435171,    2.17329,  -0.550928,  0.0729055,  0.0728801,  0.0777703,  0.0826346,  0.0873382,  0.0917822,  0.0958249,  0.0993861,   0.102344,    0.10458,   0.105989,   0.106457,   0.105951,    0.10458,   0.102478,   0.099736,  0.0964159,  0.0925584,  0.0881943,  0.0833512,  0.0780585,  0.0723507,  0.0662704,  0.0598714,  0.0532232,  0.0464104,  0.0395359,  0.0327239,  0.0261192,  0.0198866,  0.0142071, 0.00926991, 0.00525974, 0.00233253,0.000578235,
 0.0147798,  0.0152998,  0.0160959,  0.0173061,  0.0191368,  0.0218979,  0.0260536,  0.0313421,  0.0368827,  0.0423313,  0.0476839,  0.0529438,  0.0629768,  -0.550928,    2.45348,  -0.680623,  0.0836818,  0.0841347,  0.0894134,  0.0945209,  0.0993499,   0.103747,   0.107625,   0.110851,   0.113298,    0.11485,   0.115384,   0.114862,   0.113403,    0.11115,   0.108203,   0.104627,   0.100466,  0.0957534,  0.0905189,  0.0847938,  0.0786154,  0.0720293,  0.0650938,   0.057884,  0.0504916,  0.0430282,  0.0356286,  0.0284502,  0.0216723,  0.0154917,  0.0101151, 0.00574413, 0.00254997,0.000632796,
 0.0158851,  0.0164446,   0.017301,  0.0186026,  0.0205715,  0.0235408,  0.0280097,   0.033697,  0.0396564,  0.0455183,  0.0512787,  0.0569413,   0.062522,  0.0729055,  -0.680623,    2.77039,  -0.829685,  0.0954303,  0.0962819,   0.101804,   0.107028,   0.111791,   0.115997,   0.119503,    0.12217,   0.123875,   0.124483,   0.123953,    0.12241,   0.120011,   0.116861,    0.11303,   0.108565,   0.103503,  0.0978734,  0.0917107,  0.0850546,  0.0779541,  0.0704717,  0.0626883,  0.0547029,  0.0466359,  0.0386329,  0.0308643,  0.0235243,  0.0168264,   0.010995, 0.00624958,  0.0027775,0.000690051,
 0.0169949,  0.0175941,  0.0185112,  0.0199048,  0.0220127,  0.0251913,  0.0299751,  0.0360635,  0.0424442,  0.0487223,  0.0548935,  0.0609622,  0.0669457,  0.0728801,  0.0836818,  -0.829685,    3.13869,   -1.00693,   0.108069,   0.109164,   0.114795,   0.119934,   0.124479,   0.128277,   0.131176,   0.133045,   0.133735,   0.133205,   0.131587,   0.129047,   0.125698,   0.121615,   0.116848,   0.111435,   0.105409,  0.0988048,  0.0916654,  0.0840431,  0.0760046,  0.0676367,  0.0590454,  0.0503606,  0.0417388,  0.0333638,   0.025445,  0.0182132,  0.0119112, 0.00677729, 0.00301578,0.000750195,
 0.0181167,  0.0187562,  0.0197348,  0.0212216,  0.0234702,  0.0268608,  0.0319633,  0.0384577,  0.0452652,  0.0519649,  0.0585527,  0.0650338,  0.0714266,  0.0777703,  0.0841347,  0.0954303,   -1.00693,    3.55602,   -1.20139,   0.121504,   0.122705,   0.128235,   0.133135,   0.137238,   0.140384,   0.142429,   0.143215,   0.142694,   0.141008,   0.138332,   0.134788,   0.130456,   0.125386,    0.11962,   0.113193,    0.10614,  0.0985087,  0.0903531,  0.0817449,  0.0727765,  0.0635618,  0.0542396,   0.044978,  0.0359745,  0.0274547,   0.019667,  0.0128739, 0.00733329, 0.00326765,0.000813965,
 0.0192294,  0.0199091,  0.0209488,  0.0225282,  0.0249166,  0.0285178,   0.033937,  0.0408349,  0.0480667,  0.0551859,  0.0621886,  0.0690804,  0.0758818,  0.0826346,  0.0894134,  0.0962819,   0.108069,   -1.20139,    4.05652,   -1.45708,   0.135469,    0.13655,   0.141815,   0.146237,   0.149642,   0.151876,   0.152769,    0.15227,   0.150527,   0.147727,   0.143998,   0.139423,   0.134057,   0.127944,   0.121117,   0.113619,   0.105494,  0.0968027,  0.0876202,  0.0780448,  0.0681978,  0.0582276,  0.0483139,  0.0386682,  0.0295324,  0.0211736,  0.0138742, 0.00791281, 0.00353114,0.000880915,
 0.0203016,  0.0210201,   0.022119,  0.0237879,  0.0263114,  0.0301159,  0.0358409,  0.0431284,  0.0507701,  0.0582951,  0.0656994,  0.0729897,  0.0801876,  0.0873382,  0.0945209,   0.101804,   0.109164,   0.121504,   -1.45708,     4.6509,   -1.74284,   0.149512,   0.150296,   0.155043,   0.158716,   0.161151,   0.162165,   0.161703,    0.15992,   0.157012,   0.153113,   0.148313,   0.142668,   0.136222,   0.129013,   0.121081,   0.112476,    0.10326,  0.0935125,  0.0833378,  0.0728642,  0.0622496,  0.0516852,  0.0413967,  0.0316423,  0.0227078,  0.0148961, 0.00850711, 0.00380254, 0.00095017,
 0.0213104,  0.0220656,  0.0232203,  0.0249737,  0.0276246,   0.031621,  0.0376343,  0.0452894,  0.0533181,  0.0612265,   0.069011,  0.0766788,  0.0842533,  0.0917822,  0.0993499,   0.107028,   0.114795,   0.122705,   0.135469,   -1.74284,    5.34257,   -2.09315,   0.163258,   0.163483,   0.167432,   0.170079,    0.17123,   0.170822,   0.169018,   0.166024,    0.16198,   0.156978,   0.151078,   0.144323,   0.136754,   0.128413,    0.11935,    0.10963,  0.0993377,  0.0885816,  0.0774977,  0.0662526,  0.0550491,  0.0441264,  0.0337594,  0.0242525,  0.0159289, 0.00911037, 0.00407943, 0.00102117,
 0.0222229,  0.0230115,   0.024217,  0.0260471,  0.0288138,  0.0329843,  0.0392594,  0.0472481,  0.0556286,  0.0638859,  0.0720169,  0.0800295,  0.0879487,  0.0958249,   0.103747,   0.111791,   0.119934,   0.128235,    0.13655,   0.149512,   -2.09315,    6.14394,   -2.48811,   0.176156,   0.175534,   0.178403,   0.179705,   0.179373,   0.177575,   0.174524,   0.170365,   0.165195,   0.159073,   0.152046,   0.144153,   0.135438,   0.125954,   0.115767,   0.104964,  0.0936605,  0.0819982,  0.0701524,  0.0583365,  0.0468033,  0.0358432,   0.025779,  0.0169544, 0.00971254, 0.00435752, 0.00109288,
 0.0230208,   0.023839,  0.0250891,  0.0269868,  0.0298552,  0.0341787,  0.0406836,  0.0489655,  0.0576553,  0.0662202,  0.0746572,  0.0829751,  0.0912004,  0.0993861,   0.107625,   0.115997,   0.124479,   0.133135,   0.141815,   0.150296,   0.163258,   -2.48811,    7.04484,   -2.93838,    0.18772,   0.185969,    0.18744,   0.187208,   0.185445,    0.18237,   0.178134,   0.172834,   0.166532,   0.159275,   0.151103,   0.142059,   0.132198,   0.121588,    0.11032,  0.0985115,  0.0863119,  0.0739038,   0.061511,  0.0493987,  0.0378725,  0.0272728,  0.0179634,  0.0103087, 0.00463478, 0.00116485,
 0.0236764,  0.0245191,  0.0258063,    0.02776,  0.0307126,  0.0351626,  0.0418576,   0.050382,  0.0593282,  0.0681487,  0.0768408,  0.0854143,  0.0938969,   0.102344,   0.110851,   0.119503,   0.128277,   0.137238,   0.146237,   0.155043,   0.163483,   0.176156,   -2.93838,    8.06164,   -3.45104,    0.19739,   0.194197,   0.194093,   0.192399,   0.189341,   0.185071,    0.17969,   0.173259,   0.165825,   0.157428,   0.148113,   0.137933,   0.126959,   0.115283,   0.103027,  0.0903458,  0.0774286,  0.0645084,  0.0518621,  0.0398093,  0.0287073,  0.0189387,  0.0108894, 0.00490711, 0.00123609,
 0.0241627,   0.025024,  0.0263393,   0.028335,  0.0313509,   0.035896,  0.0427335,    0.05144,  0.0605793,  0.0695933,  0.0784796,  0.0872488,  0.0959298,    0.10458,   0.113298,    0.12217,   0.131176,   0.140384,   0.149642,   0.158716,   0.167432,   0.175534,    0.18772,   -3.45104,    9.19385,   -4.02047,   0.204583,   0.199785,     0.1982,   0.195206,   0.190955,   0.185549,    0.17905,   0.171504,    0.16295,   0.153431,   0.143004,   0.131737,   0.119726,   0.107095,  0.0940028,  0.0806444,  0.0672611,  0.0541398,  0.0416132,  0.0300537,  0.0198621,  0.0114444, 0.00517003,  0.0013055,
 0.0244562,  0.0253293,  0.0266622,  0.0286842,  0.0317395,  0.0363435,  0.0432693,  0.0520887,  0.0613489,   0.070485,  0.0794955,  0.0883916,  0.0972032,   0.105989,    0.11485,   0.123875,   0.133045,   0.142429,   0.151876,   0.161151,   0.170079,   0.178403,   0.185969,    0.19739,   -4.02047,     10.436,   -4.64839,   0.208904,   0.202623,   0.199745,   0.195574,   0.190208,   0.183711,   0.176126,   0.167493,   0.157853,   0.147261,   0.135787,   0.123527,   0.110607,  0.0971881,  0.0834709,  0.0697027,  0.0561793,  0.0432443,  0.0312839,  0.0207151,  0.0119632, 0.00541901, 0.00137198,
 0.0245311,  0.0254083,  0.0267469,  0.0287774,  0.0318448,  0.0364668,  0.0434191,  0.0522731,  0.0615717,  0.0707489,  0.0798038,  0.0887481,  0.0976125,   0.106457,   0.115384,   0.124483,   0.133735,   0.143215,   0.152769,   0.162165,    0.17123,   0.179705,    0.18744,   0.194197,   0.204583,   -4.64839,    11.7878,   -5.33433,   0.210254,   0.202702,   0.198678,   0.193428,   0.187013,   0.179476,   0.170853,   0.161185,   0.150527,   0.138946,   0.126538,   0.113432,  0.0997879,   0.085811,   0.071753,  0.0579164,  0.0446536,  0.0323626,  0.0214748,  0.0124328, 0.00564823, 0.00143408,
  0.024381,  0.0252543,  0.0265864,  0.0286066,  0.0316582,  0.0362558,  0.0431711,  0.0519786,  0.0612305,   0.070365,  0.0793815,  0.0882924,   0.097129,   0.105951,   0.114862,   0.123953,   0.133205,   0.142694,    0.15227,   0.161703,   0.170822,   0.179373,   0.187208,   0.194093,   0.199785,   0.208904,   -5.33433,    13.1997,   -6.03149,   0.208804,   0.200149,   0.195095,   0.188848,    0.18145,   0.172934,    0.16334,   0.152719,   0.141139,   0.128693,   0.115509,    0.10175,  0.0876202,  0.0733749,  0.0593213,  0.0458186,  0.0332737,  0.0221305,  0.0128472,   0.005855, 0.00149112,
 0.0240317,  0.0248939,  0.0262087,  0.0282022,  0.0312129,  0.0357487,  0.0425704,  0.0512594,  0.0603891,  0.0694062,  0.0783108,  0.0871155,   0.095852,    0.10458,   0.113403,    0.12241,   0.131587,   0.141008,   0.150527,    0.15992,   0.169018,   0.177575,   0.185445,   0.192399,     0.1982,   0.202623,   0.210254,   -6.03149,    14.5935,   -6.70852,   0.204957,   0.195328,   0.189334,   0.182163,   0.173847,   0.164422,   0.153936,   0.142457,   0.130075,   0.116915,   0.103141,  0.0889556,  0.0746164,  0.0604329,   0.046769,  0.0340389,  0.0226967,  0.0132147, 0.00604309, 0.00154406,
  0.023515,    0.02436,  0.0256483,  0.0276011,    0.03055,  0.0349922,  0.0416728,  0.0501825,  0.0591263,  0.0679629,  0.0766931,  0.0853299,   0.093905,   0.102478,    0.11115,   0.120011,   0.129047,   0.138332,   0.147727,   0.157012,   0.166024,   0.174524,    0.18237,   0.189341,   0.195206,   0.199745,   0.202702,   0.208804,   -6.70852,    15.9565,   -7.38212,   0.199136,   0.188627,   0.181769,   0.173739,   0.164572,   0.154313,   0.143025,   0.130797,   0.117753,   0.104051,   0.089896,  0.0755436,  0.0613048,  0.0475465,  0.0346885,  0.0231936,  0.0135471, 0.00621787, 0.00159427,
 0.0228526,  0.0236752,   0.024929,  0.0268289,  0.0296976,  0.0340185,  0.0405164,  0.0487941,   0.057496,   0.066097,  0.0745983,   0.083013,  0.0913727,   0.099736,   0.108203,   0.116861,   0.125698,   0.134788,   0.143998,   0.153113,    0.16198,   0.170365,   0.178134,   0.185071,   0.190955,   0.195574,   0.198678,   0.200149,   0.204957,   -7.38212,    17.3197,   -8.06487,   0.191653,   0.180343,   0.172687,   0.163866,   0.153921,   0.142913,   0.130926,   0.118081,   0.104535,  0.0904886,  0.0761969,  0.0619702,  0.0481772,  0.0352418,  0.0236342,  0.0138522, 0.00638299, 0.00164271,
 0.0220592,  0.0228547,  0.0240666,  0.0259027,  0.0286747,  0.0328494,  0.0391271,  0.0471248,  0.0555347,  0.0638502,  0.0720729,  0.0802164,  0.0883116,  0.0964159,   0.104627,    0.11303,   0.121615,   0.130456,   0.139423,   0.148313,   0.156978,   0.165195,   0.172834,    0.17969,   0.185549,   0.190208,   0.193428,   0.195095,   0.195328,   0.199136,   -8.06487,    18.7115,   -8.77116,   0.182748,   0.170709,   0.162325,   0.152784,   0.142144,   0.130485,   0.117925,   0.104615,  0.0907551,  0.0765955,  0.0624457,  0.0486747,  0.0357092,   0.024026,  0.0141344, 0.00654069, 0.00168998,
 0.0211448,  0.0219087,   0.023072,  0.0248341,  0.0274939,  0.0314993,   0.037522,  0.0451955,  0.0532665,  0.0612501,  0.0691483,  0.0769745,  0.0847593,  0.0925584,   0.100466,   0.108565,   0.116848,   0.125386,   0.134057,   0.142668,   0.151078,   0.159073,   0.166532,   0.173259,    0.17905,   0.183711,   0.187013,   0.188848,   0.189334,   0.188627,   0.191653,   -8.77116,    20.1563,   -9.50998,    0.17262,   0.159923,   0.150885,   0.140707,   0.129469,   0.117281,   0.104292,  0.0906966,  0.0767419,  0.0627345,  0.0490425,  0.0360942,  0.0243718,  0.0143959, 0.00669206,  0.0017364,
 0.0201169,   0.020845,  0.0219533,  0.0236318,   0.026165,  0.0299794,  0.0357144,  0.0430219,  0.0507101,   0.058318,  0.0658481,  0.0733137,  0.0807445,  0.0881943,  0.0957534,   0.103503,   0.111435,    0.11962,   0.127944,   0.136222,   0.144323,   0.152046,   0.159275,   0.165825,   0.171504,   0.176126,   0.179476,    0.18145,   0.182163,   0.181769,   0.180343,   0.182748,   -9.50998,    21.6734,   -10.2907,   0.161449,   0.148171,   0.138562,   0.127843,   0.116123,   0.103545,  0.0902988,  0.0766262,  0.0628303,  0.0492769,  0.0363948,   0.024671,  0.0146368, 0.00683738,  0.0017821,
 0.0189824,  0.0196706,   0.020718,  0.0223038,  0.0246967,  0.0282995,   0.033716,  0.0406182,  0.0478821,  0.0550729,  0.0621937,  0.0692574,  0.0762927,  0.0833512,  0.0905189,  0.0978734,   0.105409,   0.113193,   0.121117,   0.129013,   0.136754,   0.144153,   0.151103,   0.157428,    0.16295,   0.167493,   0.170853,   0.172934,   0.173847,   0.173739,   0.172687,   0.170709,    0.17262,   -10.2907,     23.279,   -11.1194,   0.149412,   0.135638,   0.125553,   0.114407,    0.10234,  0.0895345,  0.0762281,  0.0627185,  0.0493681,  0.0366052,  0.0249205,  0.0148556, 0.00697626, 0.00182704,
 0.0177483,   0.018393,  0.0193738,  0.0208584,  0.0230982,  0.0264702,  0.0315393,  0.0379994,  0.0447999,  0.0515349,  0.0582074,  0.0648304,  0.0714311,  0.0780585,  0.0847938,  0.0917107,  0.0988048,    0.10614,   0.113619,   0.121081,   0.128413,   0.135438,   0.142059,   0.148113,   0.153431,   0.157853,   0.161185,    0.16334,   0.164422,   0.164572,   0.163866,   0.162325,   0.159923,   0.161449,   -11.1194,    24.9877,   -12.0039,   0.136693,   0.122519,   0.112069,   0.100625,  0.0883636,   0.075517,  0.0623766,  0.0493005,  0.0367152,  0.0251143,  0.0150496, 0.00710765,   0.001871,
  0.016423,  0.0170208,  0.0179298,  0.0193054,  0.0213803,  0.0245038,  0.0291989,   0.035183,  0.0414843,  0.0477274,  0.0539158,   0.060062,  0.0661916,  0.0723507,  0.0786154,  0.0850546,  0.0916654,  0.0985087,   0.105494,   0.112476,    0.11935,   0.125954,   0.132198,   0.137933,   0.143004,   0.147261,   0.150527,   0.152719,   0.153936,   0.154313,   0.153921,   0.152784,   0.150885,   0.148171,   0.149412,   -12.0039,    26.8128,   -12.9489,   0.123487,   0.109022,  0.0983302,  0.0867318,  0.0744513,  0.0617739,  0.0490524,  0.0367105,  0.0252439,  0.0152141, 0.00722978, 0.00191359,
 0.0150168,  0.0155645,   0.016397,  0.0176565,  0.0195561,  0.0224152,  0.0267126,  0.0321903,  0.0379601,  0.0436792,  0.0493512,  0.0549879,  0.0606134,  0.0662704,  0.0720293,  0.0779541,  0.0840431,  0.0903531,  0.0968027,    0.10326,    0.10963,   0.115767,   0.121588,   0.126959,   0.131737,   0.135787,   0.138946,   0.141139,   0.142457,   0.143025,   0.142913,   0.142144,   0.140707,   0.138562,   0.135638,   0.136693,   -12.9489,    28.7688,   -13.9633,   0.110002,  0.0953639,   0.084566,  0.0729747,  0.0608684,  0.0485938,  0.0365709,  0.0252968,  0.0153427, 0.00733988, 0.00195418,
 0.0135421,  0.0140371,  0.0147892,  0.0159266,  0.0176418,  0.0202231,  0.0241026,  0.0290481,  0.0342589,  0.0394264,  0.0445541,  0.0496532,   0.054746,  0.0598714,  0.0650938,  0.0704717,  0.0760046,  0.0817449,  0.0876202,  0.0935125,  0.0993377,   0.104964,    0.11032,   0.115283,   0.119726,   0.123527,   0.126538,   0.128693,   0.130075,   0.130797,   0.130926,   0.130485,   0.129469,   0.127843,   0.125553,   0.122519,   0.123487,   -13.9633,    30.8685,   -15.0501,  0.0964541,  0.0817694,  0.0710128,  0.0596044,  0.0478845,  0.0362691,  0.0252561,   0.015426, 0.00743395, 0.00199177,
 0.0120153,  0.0124554,  0.0131239,  0.0141347,  0.0156585,  0.0179515,  0.0213975,  0.0257907,  0.0304212,  0.0350155,   0.039577,  0.0441162,  0.0486531,  0.0532232,   0.057884,  0.0626883,  0.0676367,  0.0727765,  0.0780448,  0.0833378,  0.0885816,  0.0936605,  0.0985115,   0.103027,   0.107095,   0.110607,   0.113432,   0.115509,   0.116915,   0.117753,   0.118081,   0.117925,   0.117281,   0.116123,   0.114407,   0.112069,   0.109022,   0.110002,   -15.0501,    33.1226,   -16.2163,  0.0830654,   0.068467,  0.0579081,  0.0468714,  0.0357688,  0.0250987,  0.0154513, 0.00750635, 0.00202496,
 0.0104558,  0.0108398,  0.0114226,  0.0123036,  0.0136316,  0.0156295,  0.0186318,  0.0224598,  0.0264961,  0.0305027,  0.0344833,  0.0384472,  0.0424125,  0.0464104,  0.0504916,  0.0547029,  0.0590454,  0.0635618,  0.0681978,  0.0728642,  0.0774977,  0.0819982,  0.0863119,  0.0903458,  0.0940028,  0.0971881,  0.0997879,    0.10175,   0.103141,   0.104051,   0.104535,   0.104615,   0.104292,   0.103545,    0.10234,   0.100625,  0.0983302,  0.0953639,  0.0964541,   -16.2163,    35.5474,   -17.4702,  0.0700533,  0.0556778,  0.0454807,  0.0350191,  0.0247921,     0.0154, 0.00754876, 0.00205164,
 0.0088874, 0.00921461, 0.00971106,  0.0104612,  0.0115917,  0.0132923,  0.0158475,  0.0191059,  0.0225428,  0.0259565,  0.0293502,  0.0327323,  0.0361186,  0.0395359,  0.0430282,  0.0466359,  0.0503606,  0.0542396,  0.0582276,  0.0622496,  0.0662526,  0.0701524,  0.0739038,  0.0774286,  0.0806444,  0.0834709,   0.085811,  0.0876202,  0.0889556,   0.089896,  0.0904886,  0.0907551,  0.0906966,  0.0902988,  0.0895345,  0.0883636,  0.0867318,   0.084566,  0.0817694,  0.0830654,   -17.4702,    38.1578,   -18.8176,  0.0576234,  0.0436095,  0.0339484,  0.0242901,  0.0152458, 0.00754909, 0.00206867,
0.00733827, 0.00760922, 0.00802008, 0.00864067, 0.00957567,   0.010982,  0.0130948,  0.0157894,  0.0186328,  0.0214587,  0.0242702,  0.0270744,  0.0298847,  0.0327239,  0.0356286,  0.0386329,  0.0417388,   0.044978,  0.0483139,  0.0516852,  0.0550491,  0.0583365,   0.061511,  0.0645084,  0.0672611,  0.0697027,   0.071753,  0.0733749,  0.0746164,  0.0755436,  0.0761969,  0.0765955,  0.0767419,  0.0766262,  0.0762281,   0.075517,  0.0744513,  0.0729747,  0.0710128,   0.068467,  0.0700533,   -18.8176,    40.9673,   -20.2652,  0.0459644,   0.032456,  0.0235272,  0.0149508, 0.00748977, 0.00207144,
0.00584136, 0.00605773, 0.00638561, 0.00688068, 0.00762634, 0.00874769,  0.0104322,  0.0125808,  0.0148492,  0.0171051,  0.0193513,  0.0215938,  0.0238436,  0.0261192,  0.0284502,  0.0308643,  0.0333638,  0.0359745,  0.0386682,  0.0413967,  0.0441264,  0.0468033,  0.0493987,  0.0518621,  0.0541398,  0.0561793,  0.0579164,  0.0593213,  0.0604329,  0.0613048,  0.0619702,  0.0624457,  0.0627345,  0.0628303,  0.0627185,  0.0623766,  0.0617739,  0.0608684,  0.0596044,  0.0579081,  0.0556778,  0.0576234,   -20.2652,    43.9911,   -21.8209,  0.0352484,  0.0224088,  0.0144597, 0.00734497, 0.00205314,
0.00443384, 0.00459867, 0.00484827, 0.00522496, 0.00579217, 0.00664496, 0.00792588, 0.00955997,  0.0112861,   0.013004,  0.0147161,  0.0164272,  0.0181459,  0.0198866,  0.0216723,  0.0235243,   0.025445,  0.0274547,  0.0295324,  0.0316423,  0.0337594,  0.0358432,  0.0378725,  0.0398093,  0.0416132,  0.0432443,  0.0446536,  0.0458186,   0.046769,  0.0475465,  0.0481772,  0.0486747,  0.0490425,  0.0492769,  0.0493681,  0.0493005,  0.0490524,  0.0485938,  0.0478845,  0.0468714,  0.0454807,  0.0436095,  0.0459644,   -21.8209,    47.2464,    -23.493,  0.0256481,  0.0136917, 0.00707654,  0.0020036,
0.00315622, 0.00327405, 0.00345233, 0.00372124, 0.00412601, 0.00473444, 0.00564818, 0.00681406, 0.00804635, 0.00927395,  0.0104987,  0.0117241,  0.0129568,  0.0142071,  0.0154917,  0.0168264,  0.0182132,   0.019667,  0.0211736,  0.0227078,  0.0242525,   0.025779,  0.0272728,  0.0287073,  0.0300537,  0.0312839,  0.0323626,  0.0332737,  0.0340389,  0.0346885,  0.0352418,  0.0357092,  0.0360942,  0.0363948,  0.0366052,  0.0367152,  0.0367105,  0.0365709,  0.0362691,  0.0357688,  0.0350191,  0.0339484,   0.032456,  0.0352484,    -23.493,    50.7532,   -25.2925,  0.0173783, 0.00662773, 0.00190766,
0.00205059, 0.00212753, 0.00224383, 0.00241914,  0.0026829, 0.00307927, 0.00367441, 0.00443396, 0.00523737, 0.00603857,  0.0068389, 0.00764089, 0.00844888, 0.00926991,  0.0101151,   0.010995,  0.0119112,  0.0128739,  0.0138742,  0.0148961,  0.0159289,  0.0169544,  0.0179634,  0.0189387,  0.0198621,  0.0207151,  0.0214748,  0.0221305,  0.0226967,  0.0231936,  0.0236342,   0.024026,  0.0243718,   0.024671,  0.0249205,  0.0251143,  0.0252439,  0.0252968,  0.0252561,  0.0250987,  0.0247921,  0.0242901,  0.0235272,  0.0224088,  0.0256481,   -25.2925,    54.5308,   -27.2266,  0.0107653, 0.00174259,
0.00115742, 0.00120111, 0.00126708, 0.00136645, 0.00151587, 0.00174033, 0.00207729, 0.00250744, 0.00296285,  0.0034176, 0.00387255, 0.00432925, 0.00479027, 0.00525974, 0.00574413, 0.00624958, 0.00677729, 0.00733329, 0.00791281, 0.00850711, 0.00911037, 0.00971254,  0.0103087,  0.0108894,  0.0114444,  0.0119632,  0.0124328,  0.0128472,  0.0132147,  0.0135471,  0.0138522,  0.0141344,  0.0143959,  0.0146368,  0.0148556,  0.0150496,  0.0152141,  0.0153427,   0.015426,  0.0154513,     0.0154,  0.0152458,  0.0149508,  0.0144597,  0.0136917,  0.0173783,   -27.2266,    58.5965,   -29.3045, 0.00632577,
0.000509977,0.000529376,0.000558621,0.000602627,0.000668762,0.000768069,0.000917103, 0.00110742, 0.00130914, 0.00151089,  0.0017131, 0.00191653, 0.00212237, 0.00233253, 0.00254997,  0.0027775, 0.00301578, 0.00326765, 0.00353114, 0.00380254, 0.00407943, 0.00435752, 0.00463478, 0.00490711, 0.00517003, 0.00541901, 0.00564823,   0.005855, 0.00604309, 0.00621787, 0.00638299, 0.00654069, 0.00669206, 0.00683738, 0.00697626, 0.00710765, 0.00722978, 0.00733988, 0.00743395, 0.00750635, 0.00754876, 0.00754909, 0.00748977, 0.00734497, 0.00707654, 0.00662773,  0.0107653,   -29.3045,    62.9714,   -31.5314,
0.000125592,0.000130406,0.000137654,0.000148548,0.000164911, 0.00018947,0.000226316,0.000273385,0.000323329,0.000373363,0.000423607,0.000474262,0.000525641,0.000578235,0.000632796,0.000690051,0.000750195,0.000813965,0.000880915, 0.00095017, 0.00102117, 0.00109288, 0.00116485, 0.00123609,  0.0013055, 0.00137198, 0.00143408, 0.00149112, 0.00154406, 0.00159427, 0.00164271, 0.00168998,  0.0017364,  0.0017821, 0.00182704,   0.001871, 0.00191359, 0.00195418, 0.00199177, 0.00202496, 0.00205164, 0.00206867, 0.00207144, 0.00205314,  0.0020036, 0.00190766, 0.00174259, 0.00632577,   -31.5314,    33.7455).finished();

const Eigen::MatrixXd f = (Eigen::MatrixXd(1,50) << -0.0111425,
  -0.011538,
 -0.0121425,
 -0.0130601,
 -0.0144474,
 -0.0165385,
 -0.0196848,
 -0.0236903,
 -0.0278921,
 -0.0320323,
  -0.036109,
 -0.0401265,
 -0.0440975,
 -0.0480478,
 -0.0520223,
 -0.0560601,
 -0.0601514,
 -0.0643271,
  -0.068518,
 -0.0726177,
 -0.0765486,
 -0.0801968,
 -0.0834977,
 -0.0863953,
 -0.0885252,
  -0.101229,
  -0.103082,
  -0.104449,
  -0.105031,
  -0.104906,
  -0.104092,
  -0.102617,
  -0.100496,
 -0.0977022,
 -0.0942305,
 -0.0900319,
 -0.0851233,
 -0.0794254,
 -0.0732799,
  -0.052512,
 -0.0469537,
 -0.0409763,
 -0.0349491,
 -0.0289862,
 -0.0229266,
 -0.0388009,
 -0.0517509,
 -0.0746045,
  -0.109363,
  -0.157534).finished();


const Eigen::MatrixXd A = (Eigen::MatrixXd(50,50) <<  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
 -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1,  0,
  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, -1,  1).finished();


const Eigen::MatrixXd lb = (Eigen::MatrixXd(50,1) <<  -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7,
 -0.7).finished();

const Eigen::MatrixXd ub = (Eigen::MatrixXd(50,1) << 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7,
 0.7).finished();

const Eigen::MatrixXd lbA = (Eigen::MatrixXd(50,1) << -0.022612,
 -0.0943183,
 -0.0947408,
 -0.0951632,
 -0.0955856,
 -0.0960081,
 -0.0964305,
 -0.0968748,
 -0.0973306,
 -0.0977864,
 -0.0982422,
 -0.0986979,
 -0.0991537,
 -0.0996095,
  -0.100065,
  -0.100901,
  -0.102576,
  -0.104252,
  -0.104797,
  -0.105076,
  -0.105429,
  -0.105815,
  -0.106251,
  -0.106762,
  -0.107324,
  -0.107944,
  -0.108627,
  -0.109345,
  -0.110009,
  -0.110571,
  -0.110995,
  -0.111277,
  -0.111414,
  -0.111385,
  -0.111186,
  -0.110788,
  -0.110203,
  -0.109409,
  -0.108455,
  -0.107447,
  -0.106401,
  -0.105314,
  -0.101557,
 -0.0946047,
  -0.104894,
  -0.109788,
  -0.118091,
  -0.125476,
  -0.125476,
  -0.125476).finished();


const Eigen::MatrixXd ubA = (Eigen::MatrixXd(50,1) << 0.0244062,
 0.0943183,
 0.0947408,
 0.0951632,
 0.0955856,
 0.0960081,
 0.0964305,
 0.0968748,
 0.0973306,
 0.0977864,
 0.0982422,
 0.0986979,
 0.0991537,
 0.0996095,
  0.100065,
  0.100901,
  0.102576,
  0.104252,
  0.104797,
  0.105076,
  0.105429,
  0.105815,
  0.106251,
  0.106762,
  0.107324,
  0.107944,
  0.108627,
  0.109345,
  0.110009,
  0.110571,
  0.110995,
  0.111277,
  0.111414,
  0.111385,
  0.111186,
  0.110788,
  0.110203,
  0.109409,
  0.108455,
  0.107447,
  0.106401,
  0.105314,
  0.101557,
 0.0946047,
  0.104894,
  0.109788,
  0.118091,
  0.125476,
  0.125476,
  0.125476).finished();

  auto t_start = std::chrono::system_clock::now();
  bool solve_result = m_qpsolver_ptr->solve(H, f.transpose(), A, lb, ub, lbA, ubA, Uex);
  std::cerr << "H = " << H << std::endl;
  std::cerr << "f.transpose() = " << f.transpose() << std::endl;
  std::cerr << "A = " << A << std::endl;
  std::cerr << "lb = " << lb << std::endl;
  std::cerr << "ub = " << ub << std::endl;
  std::cerr << "lbA = " << lbA << std::endl;
  std::cerr << "ubA = " << ubA << std::endl;
  std::cerr << "solve_result: " << solve_result << std::endl;
  auto t_end = std::chrono::system_clock::now();
  if (!solve_result) {
    warn_throttle("qp solver error");
    return {false, {}};
  }

  {
    auto t = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    RCLCPP_DEBUG(m_logger, "qp solver calculation time = %ld [ms]", t);
  }

  if (Uex.array().isNaN().any()) {
    warn_throttle("model Uex includes NaN, stop MPC.");
    return {false, {}};
  }
  return {true, Uex};
}

void MPC::addSteerWeightR(const double prediction_dt, MatrixXd & R) const
{
  const int N = m_param.prediction_horizon;
  const double DT = prediction_dt;

  // add steering rate : weight for (u(i) - u(i-1) / dt )^2
  {
    const double steer_rate_r = m_param.nominal_weight.steer_rate / (DT * DT);
    const Eigen::Matrix2d D = steer_rate_r * (Eigen::Matrix2d() << 1.0, -1.0, -1.0, 1.0).finished();
    for (int i = 0; i < N - 1; ++i) {
      R.block(i, i, 2, 2) += D;
    }
    if (N > 1) {
      // steer rate i = 0
      R(0, 0) += m_param.nominal_weight.steer_rate / (m_ctrl_period * m_ctrl_period);
    }
  }

  // add steering acceleration : weight for { (u(i+1) - 2*u(i) + u(i-1)) / dt^2 }^2
  {
    const double w = m_param.nominal_weight.steer_acc;
    const double steer_acc_r = w / std::pow(DT, 4);
    const double steer_acc_r_cp1 = w / (std::pow(DT, 3) * m_ctrl_period);
    const double steer_acc_r_cp2 = w / (std::pow(DT, 2) * std::pow(m_ctrl_period, 2));
    const double steer_acc_r_cp4 = w / std::pow(m_ctrl_period, 4);
    const Eigen::Matrix3d D =
      steer_acc_r *
      (Eigen::Matrix3d() << 1.0, -2.0, 1.0, -2.0, 4.0, -2.0, 1.0, -2.0, 1.0).finished();
    for (int i = 1; i < N - 1; ++i) {
      R.block(i - 1, i - 1, 3, 3) += D;
    }
    if (N > 1) {
      // steer acc i = 1
      R(0, 0) += steer_acc_r * 1.0 + steer_acc_r_cp2 * 1.0 + steer_acc_r_cp1 * 2.0;
      R(1, 0) += steer_acc_r * -1.0 + steer_acc_r_cp1 * -1.0;
      R(0, 1) += steer_acc_r * -1.0 + steer_acc_r_cp1 * -1.0;
      R(1, 1) += steer_acc_r * 1.0;
      // steer acc i = 0
      R(0, 0) += steer_acc_r_cp4 * 1.0;
    }
  }
}

void MPC::addSteerWeightF(const double prediction_dt, MatrixXd & f) const
{
  if (f.rows() < 2) {
    return;
  }

  const double DT = prediction_dt;

  // steer rate for i = 0
  f(0, 0) += -2.0 * m_param.nominal_weight.steer_rate / (std::pow(DT, 2)) * 0.5;

  // const double steer_acc_r = m_param.weight_steer_acc / std::pow(DT, 4);
  const double steer_acc_r_cp1 =
    m_param.nominal_weight.steer_acc / (std::pow(DT, 3) * m_ctrl_period);
  const double steer_acc_r_cp2 =
    m_param.nominal_weight.steer_acc / (std::pow(DT, 2) * std::pow(m_ctrl_period, 2));
  const double steer_acc_r_cp4 = m_param.nominal_weight.steer_acc / std::pow(m_ctrl_period, 4);

  // steer acc  i = 0
  f(0, 0) += ((-2.0 * m_raw_steer_cmd_prev + m_raw_steer_cmd_pprev) * steer_acc_r_cp4) * 0.5;

  // steer acc for i = 1
  f(0, 0) += (-2.0 * m_raw_steer_cmd_prev * (steer_acc_r_cp1 + steer_acc_r_cp2)) * 0.5;
  f(0, 1) += (2.0 * m_raw_steer_cmd_prev * steer_acc_r_cp1) * 0.5;
}

double MPC::getPredictionDeltaTime(
  const double start_time, const MPCTrajectory & input, const Odometry & current_kinematics) const
{
  // Calculate the time min_prediction_length ahead from current_pose
  const auto autoware_traj = MPCUtils::convertToAutowareTrajectory(input);
  const size_t nearest_idx = motion_utils::findFirstNearestIndexWithSoftConstraints(
    autoware_traj.points, current_kinematics.pose.pose, ego_nearest_dist_threshold,
    ego_nearest_yaw_threshold);
  double sum_dist = 0;
  const double target_time = [&]() {
    const double t_ext = 100.0;  // extra time to prevent mpc calculation failure due to short time
    for (size_t i = nearest_idx + 1; i < input.relative_time.size(); i++) {
      const double segment_dist = MPCUtils::calcDistance2d(input, i, i - 1);
      sum_dist += segment_dist;
      if (m_param.min_prediction_length < sum_dist) {
        const double prev_sum_dist = sum_dist - segment_dist;
        const double ratio = (m_param.min_prediction_length - prev_sum_dist) / segment_dist;
        const double relative_time_at_i = i == input.relative_time.size() - 1
                                            ? input.relative_time.at(i) - t_ext
                                            : input.relative_time.at(i);
        return input.relative_time.at(i - 1) +
               (relative_time_at_i - input.relative_time.at(i - 1)) * ratio;
      }
    }
    return input.relative_time.back() - t_ext;
  }();

  // Calculate delta time for min_prediction_length
  const double dt =
    (target_time - start_time) / static_cast<double>(m_param.prediction_horizon - 1);

  return std::max(dt, m_param.prediction_dt);
}

double MPC::calcDesiredSteeringRate(
  const MPCMatrix & mpc_matrix, const MatrixXd & x0, const MatrixXd & Uex, const double u_filtered,
  const float current_steer, const double predict_dt) const
{
  if (m_vehicle_model_ptr->modelName() != "kinematics") {
    // not supported yet. Use old implementation.
    return (u_filtered - current_steer) / predict_dt;
  }

  // calculate predicted states to get the steering motion
  const auto & m = mpc_matrix;
  const MatrixXd Xex = m.Aex * x0 + m.Bex * Uex + m.Wex;

  const size_t STEER_IDX = 2;  // for kinematics model

  const auto steer_0 = x0(STEER_IDX, 0);
  const auto steer_1 = Xex(STEER_IDX, 0);

  const auto steer_rate = (steer_1 - steer_0) / predict_dt;

  return steer_rate;
}

VectorXd MPC::calcSteerRateLimitOnTrajectory(
  const MPCTrajectory & trajectory, const double current_velocity) const
{
  const auto interp = [&](const auto & steer_rate_limit_map, const auto & current) {
    std::vector<double> reference, limits;
    for (const auto & p : steer_rate_limit_map) {
      reference.push_back(p.first);
      limits.push_back(p.second);
    }

    // If the speed is out of range of the reference, apply zero-order hold.
    if (current <= reference.front()) {
      return limits.front();
    }
    if (current >= reference.back()) {
      return limits.back();
    }

    // Apply linear interpolation
    for (size_t i = 0; i < reference.size() - 1; ++i) {
      if (reference.at(i) <= current && current <= reference.at(i + 1)) {
        auto ratio =
          (current - reference.at(i)) / std::max(reference.at(i + 1) - reference.at(i), 1.0e-5);
        ratio = std::clamp(ratio, 0.0, 1.0);
        const auto interp = limits.at(i) + ratio * (limits.at(i + 1) - limits.at(i));
        return interp;
      }
    }

    std::cerr << "MPC::calcSteerRateLimitOnTrajectory() interpolation logic is broken. Command "
                 "filter is not working. Please check the code."
              << std::endl;
    return reference.back();
  };

  // when the vehicle is stopped, no steering rate limit.
  const bool is_vehicle_stopped = std::fabs(current_velocity) < 0.01;
  if (is_vehicle_stopped) {
    return VectorXd::Zero(m_param.prediction_horizon);
  }

  // calculate steering rate limit
  VectorXd steer_rate_limits = VectorXd::Zero(m_param.prediction_horizon);
  for (int i = 0; i < m_param.prediction_horizon; ++i) {
    const auto limit_by_curvature = interp(m_steer_rate_lim_map_by_curvature, trajectory.k.at(i));
    const auto limit_by_velocity = interp(m_steer_rate_lim_map_by_velocity, trajectory.vx.at(i));
    steer_rate_limits(i) = std::min(limit_by_curvature, limit_by_velocity);
  }

  return steer_rate_limits;
}

Trajectory MPC::calculatePredictedTrajectory(
  const MPCMatrix & mpc_matrix, const Eigen::MatrixXd & x0, const Eigen::MatrixXd & Uex,
  const MPCTrajectory & reference_trajectory, const double dt) const
{
  const auto predicted_mpc_trajectory =
    m_vehicle_model_ptr->calculatePredictedTrajectoryInWorldCoordinate(
      mpc_matrix.Aex, mpc_matrix.Bex, mpc_matrix.Cex, mpc_matrix.Wex, x0, Uex, reference_trajectory,
      dt);

  // do not over the reference trajectory
  const auto predicted_length = MPCUtils::calcMPCTrajectoryArcLength(reference_trajectory);
  const auto clipped_trajectory =
    MPCUtils::clipTrajectoryByLength(predicted_mpc_trajectory, predicted_length);

  const auto predicted_trajectory = MPCUtils::convertToAutowareTrajectory(clipped_trajectory);

  // Publish trajectory in relative coordinate for debug purpose.
  if (m_debug_publish_predicted_trajectory) {
    const auto frenet = m_vehicle_model_ptr->calculatePredictedTrajectoryInFrenetCoordinate(
      mpc_matrix.Aex, mpc_matrix.Bex, mpc_matrix.Cex, mpc_matrix.Wex, x0, Uex, reference_trajectory,
      dt);
    const auto frenet_clipped = MPCUtils::convertToAutowareTrajectory(
      MPCUtils::clipTrajectoryByLength(frenet, predicted_length));
    m_debug_frenet_predicted_trajectory_pub->publish(frenet_clipped);
  }

  return predicted_trajectory;
}

bool MPC::isValid(const MPCMatrix & m) const
{
  if (
    m.Aex.array().isNaN().any() || m.Bex.array().isNaN().any() || m.Cex.array().isNaN().any() ||
    m.Wex.array().isNaN().any() || m.Qex.array().isNaN().any() || m.R1ex.array().isNaN().any() ||
    m.R2ex.array().isNaN().any() || m.Uref_ex.array().isNaN().any()) {
    return false;
  }

  if (
    m.Aex.array().isInf().any() || m.Bex.array().isInf().any() || m.Cex.array().isInf().any() ||
    m.Wex.array().isInf().any() || m.Qex.array().isInf().any() || m.R1ex.array().isInf().any() ||
    m.R2ex.array().isInf().any() || m.Uref_ex.array().isInf().any()) {
    return false;
  }

  return true;
}
}  // namespace autoware::motion::control::mpc_lateral_controller
