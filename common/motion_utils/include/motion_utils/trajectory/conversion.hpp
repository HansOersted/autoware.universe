// Copyright 2021 Tier IV, Inc.
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

#ifndef MOTION_UTILS__TRAJECTORY__CONVERSION_HPP_
#define MOTION_UTILS__TRAJECTORY__CONVERSION_HPP_

#include "autoware_auto_planning_msgs/msg/detail/path__struct.hpp"
#include "autoware_auto_planning_msgs/msg/detail/path_with_lane_id__struct.hpp"
#include "autoware_auto_planning_msgs/msg/detail/trajectory__struct.hpp"
#include "autoware_auto_planning_msgs/msg/detail/trajectory_point__struct.hpp"

#include <vector>

namespace motion_utils
{
using TrajectoryPoints = std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint>;

/**
 * @brief Convert std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> to
 * autoware_auto_planning_msgs::msg::Trajectory. This function is temporarily added for porting to
 * autoware_auto_msgs. We should consider whether to remove this function after the porting is done.
 * @attention This function just clips
 * std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> up to the capacity of Trajectory.
 * Therefore, the error handling out of this function is necessary if the size of the input greater
 * than the capacity.
 * @todo Decide how to handle the situation that we need to use the trajectory with the size of
 * points larger than the capacity. (Tier IV)
 */
autoware_auto_planning_msgs::msg::Trajectory convertToTrajectory(
  const std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> & trajectory);

/**
 * @brief Convert autoware_auto_planning_msgs::msg::Trajectory to
 * std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint>.
 */
std::vector<autoware_auto_planning_msgs::msg::TrajectoryPoint> convertToTrajectoryPointArray(
  const autoware_auto_planning_msgs::msg::Trajectory & trajectory);

template <class T>  // previous type: autoware_auto_planning_msgs::msg::PathWithLaneId
autoware_auto_planning_msgs::msg::Path convertToPath(const T & input)
{
  autoware_auto_planning_msgs::msg::Path output{};
  output.header = input.header;
  output.left_bound = input.left_bound;
  output.right_bound = input.right_bound;
  output.points.resize(input.points.size());
  for (size_t i = 0; i < input.points.size(); ++i) {
    output.points.at(i) = input.points.at(i).point;
  }
  return output;
}

template <class T>  // previous type: autoware_auto_planning_msgs::msg::PathWithLaneId
TrajectoryPoints convertToTrajectoryPoints(const T & path)
{
  TrajectoryPoints tps;
  for (const auto & p : path.points) {
    autoware_auto_planning_msgs::msg::TrajectoryPoint tp;
    tp.pose = p.point.pose;
    tp.longitudinal_velocity_mps = p.point.longitudinal_velocity_mps;
    // since path point doesn't have acc for now
    tp.acceleration_mps2 = 0;
    tps.emplace_back(tp);
  }
  return tps;
}

template <class T>  // previous type: TrajectoryPoints
autoware_auto_planning_msgs::msg::PathWithLaneId convertTrajectoryPointsToPath(const T & trajectory)
{
  autoware_auto_planning_msgs::msg::PathWithLaneId path;
  for (const auto & p : trajectory) {
    autoware_auto_planning_msgs::msg::PathPointWithLaneId pp;
    pp.point.pose = p.pose;
    pp.point.longitudinal_velocity_mps = p.longitudinal_velocity_mps;
    path.points.emplace_back(pp);
  }
  return path;
}

}  // namespace motion_utils

#endif  // MOTION_UTILS__TRAJECTORY__CONVERSION_HPP_
