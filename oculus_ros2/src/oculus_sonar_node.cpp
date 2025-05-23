/**
 * BSD 3-Clause License
 *
 * Copyright (c) 2022, ENSTA-Bretagne
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define BOOST_BIND_GLOBAL_PLACEHOLDERS

#include <oculus_ros2/oculus_sonar_node.hpp>

using SonarDriver = oculus::SonarDriver;

OculusSonarNode::OculusSonarNode()
  : Node("oculus_sonar"),
    is_running_(this->declare_parameter<bool>("run", params::RUN_MODE_DEFAULT_VALUE)),
    sonar_viewer_(static_cast<rclcpp::Node*>(this)),
    frame_id_(this->declare_parameter<std::string>("frame_id", "sonar")),
    temperature_warn_limit_(this->declare_parameter<double>("temperature_warn", params::TEMPERATURE_WARN_DEFAULT_VALUE)),
    temperature_stop_limit_(this->declare_parameter<double>("temperature_stop", params::TEMPERATURE_STOP_DEFAULT_VALUE)) {
  this->status_publisher_ = this->create_publisher<oculus_interfaces::msg::OculusStatus>("status", 1);
  this->ping_publisher_ = this->create_publisher<oculus_interfaces::msg::Ping>("ping", 1);
  this->temperature_publisher_ = this->create_publisher<sensor_msgs::msg::Temperature>("temperature", 1);
  this->pressure_publisher_ = this->create_publisher<sensor_msgs::msg::FluidPressure>("pressure", 1);

  this->sonar_driver_ = std::make_shared<SonarDriver>(this->io_service_.io_service());
  this->io_service_.start();
  if (!this->sonar_driver_->wait_next_message()) {  // Non-blocking function making connection with the sonar.
    std::cerr << "Timeout reached while waiting for a connection to the Oculus sonar. "
              << "Is it properly connected ?" << std::endl;
  }

  while (!this->sonar_driver_->connected())  // Blocking while waiting the connected with the sonar.
  {
    const int sleepWhileConnecting = 1000;
    std::this_thread::sleep_for(std::chrono::milliseconds(sleepWhileConnecting));
  }

  for (const params::BoolParam& param : params::BOOL) {
    if (!this->has_parameter(param.name)) {
      rcl_interfaces::msg::ParameterDescriptor param_desc;
      param_desc.name = param.name;
      param_desc.type = rclcpp::ParameterType::PARAMETER_BOOL;
      param_desc.description = param.desc;
      this->declare_parameter<bool>(param.name, param.default_val, param_desc);
    }
  }
  for (const params::IntParam& param : params::INT) {
    if (!this->has_parameter(param.name)) {
      rcl_interfaces::msg::ParameterDescriptor param_desc;
      param_desc.name = param.name;
      param_desc.type = rclcpp::ParameterType::PARAMETER_INTEGER;
      param_desc.description = param.desc;
      rcl_interfaces::msg::IntegerRange range;
      range.set__from_value(param.min).set__to_value(param.max).set__step(1);
      param_desc.integer_range = {range};
      this->declare_parameter<int>(param.name, param.default_val, param_desc);
    }
  }
  for (const params::DoubleParam& param : params::DOUBLE) {
    if (!this->has_parameter(param.name)) {
      rcl_interfaces::msg::ParameterDescriptor param_desc;
      param_desc.name = param.name;
      param_desc.type = rclcpp::ParameterType::PARAMETER_DOUBLE;
      param_desc.description = param.desc;
      rcl_interfaces::msg::IntegerRange range;
      range.set__from_value(param.min).set__to_value(param.max).set__step(param.step);
      param_desc.integer_range = {range};
      this->declare_parameter<double>(param.name, param.default_val, param_desc);
    }
  }

  // Get the current sonar config
  updateLocalParameters(currentSonarParameters_, this->sonar_driver_->current_ping_config());
  for (const std::string& param_name : dynamic_parameters_names_) {
    setConfigCallback(this->get_parameters(std::vector{param_name}));
  }
  this->param_cb_ = this->add_on_set_parameters_callback(std::bind(&OculusSonarNode::setConfigCallback, this,
      std::placeholders::_1));  // TODO(hugoyvrn, to move before parameters initialisation ?)

  // this->??(&OculusSonarNode::enableRunMode)  // TODO(hugoyvrn)

  this->sonar_driver_->add_status_callback(std::bind(&OculusSonarNode::publishStatus, this, std::placeholders::_1));
  this->sonar_driver_->add_ping_callback(std::bind(&OculusSonarNode::publishPing, this, std::placeholders::_1));
  // callback on dummy messages to reactivate the pings as needed
  this->sonar_driver_->add_dummy_callback(std::bind(&OculusSonarNode::handleDummy, this));
}

OculusSonarNode::~OculusSonarNode() {
  this->io_service_.stop();
}

void OculusSonarNode::enableRunMode() {
  this->sonar_driver_->resume();  // Quitting sonar standby mode
  this->set_parameter(rclcpp::Parameter("run", true));  // Important to set before is_running_
  is_running_ = true;  // Important to set after "run" ros parameter
}

void OculusSonarNode::disableRunMode() {
  this->sonar_driver_->standby();  // Going in sonar standby mode
  this->set_parameter(rclcpp::Parameter("run", false));  // Important to set before is_running_
  is_running_ = false;  // Important to set after "run" ros parameter
  RCLCPP_INFO(this->get_logger(), "Going to standby mode");
}

void OculusSonarNode::checkOverheating(const double& new_temperature) {
  is_overheating_ = new_temperature >= temperature_stop_limit_;
}

void OculusSonarNode::setMinimalFlags(uint8_t& flags) const {
  flags |= flagByte::RANGE_AS_METERS  // always in meters
         | flagByte::SEND_GAINS  // force send gain to true this
         | flagByte::SIMPLE_PING;  // use simple ping

  if (currentSonarParameters_.frequency_mode == params::FREQUENCY_MODE.max) {
    // TODO(hugoyvrn, gain_assist not working, to fix)
    // flags |= flagByte::GAIN_ASSIST;
    flags &= ~flagByte::GAIN_ASSIST;
  }

  // flags | 0x02 make wird change (depending of the configuration)
  // flags |= 0x02;
  flags &= ~0x02;
  // flags | 0x20 must be ???
  // flags |= 0x20;
  // flags &= ~0x20;
  // flags | 0x80 must be false to avoid broken connection (Header reception error) and very long param answers. Restart of the
  // sonar needed (even for IHM). tested with flags = 4d and flags = cd and flags = fd and flags = 19
  // flags |= 0x80;
  flags &= ~0x80;
}

void OculusSonarNode::checkMinimalFlags(const uint8_t& flags) const {
  if (!(flags & flagByte::RANGE_AS_METERS)) {
    RCLCPP_ERROR(get_logger(), "Range is attepreted as percent while ros driver assume range is interpreted as meters.");
  }
  if (!(flags & flagByte::SEND_GAINS)) {
    RCLCPP_ERROR(get_logger(), "The sonar don't send gain while ros driver assume gains are sended. Data is incomplete.");
  }
  if (!(flags & flagByte::SIMPLE_PING)) {
    RCLCPP_ERROR(get_logger(), "The sonar don't use simple ping message while ros driver assume simple ping are used.");
  }
}

void OculusSonarNode::publishStatus(const OculusStatusMsg& status) {

  static oculus_interfaces::msg::OculusStatus msg;
  oculus::toMsg(msg, status);
  this->status_publisher_->publish(msg);

  if (!is_running_) {
    checkOverheating(status.temperature6);
    sensor_msgs::msg::Temperature temperature_ros_msg;
    temperature_ros_msg.header.frame_id = frame_id_;
    temperature_ros_msg.header.stamp = this->now();
    temperature_ros_msg.temperature = status.temperature6;  // Measurement of the Temperature in Degrees Celsius
    temperature_ros_msg.variance = 0;  // 0 is interpreted as variance unknown
    this->temperature_publisher_->publish(temperature_ros_msg);

    sensor_msgs::msg::FluidPressure pressure_ros_msg;
    pressure_ros_msg.header.frame_id = frame_id_;
    pressure_ros_msg.header.stamp = this->now();
    pressure_ros_msg.fluid_pressure = status.pressure;  // Pressure reading in Pascals.
    pressure_ros_msg.variance = 0;  // 0 is interpreted as variance unknown
    this->pressure_publisher_->publish(pressure_ros_msg);
  }
}

void OculusSonarNode::updateRosConfig() {
  std::shared_lock l(param_mutex_);

  updateRosConfigForParam<int>(
      currentRosParameters_.frequency_mode, currentSonarParameters_.frequency_mode, params::FREQUENCY_MODE.name);
  updateRosConfigForParam<double>(currentRosParameters_.range, currentSonarParameters_.range, params::RANGE.name);
  updateRosConfigForParam<double>(
      currentRosParameters_.gain_percent, currentSonarParameters_.gain_percent, params::GAIN_PERCENT.name);
  updateRosConfigForParam<double>(
      currentRosParameters_.sound_speed, currentSonarParameters_.sound_speed, params::SOUND_SPEED.name);
  updateRosConfigForParam<int>(currentRosParameters_.ping_rate, currentSonarParameters_.ping_rate, params::PING_RATE.name);
  updateRosConfigForParam<bool>(currentRosParameters_.gain_assist, currentSonarParameters_.gain_assist, params::GAIN_ASSIT.name);
  updateRosConfigForParam<int>(
      currentRosParameters_.gamma_correction, currentSonarParameters_.gamma_correction, params::GAMMA_CORRECTION.name);
  updateRosConfigForParam<bool>(
      currentRosParameters_.use_salinity, currentSonarParameters_.use_salinity, params::USE_SALINITY.name);
  updateRosConfigForParam<double>(currentRosParameters_.salinity, currentSonarParameters_.salinity, params::SALINITY.name);
}

int OculusSonarNode::get_subscription_count() const {
  return this->ping_publisher_->get_subscription_count() + sonar_viewer_.image_publisher_->get_subscription_count();
}

void OculusSonarNode::publishPing(const oculus::PingMessage::ConstPtr& ping) {
  // Check if the sonar must go in standby mode
  checkOverheating(ping->temperature());
  if (!is_running_) {
    disableRunMode();
  } else if (get_subscription_count() == 0) {
    RCLCPP_INFO(this->get_logger(), "There is no subscriber nor to ping topic neither to image topic.");
    disableRunMode();
  } else if (currentSonarParameters_.ping_rate == pingRateStandby) {
    RCLCPP_INFO_STREAM(this->get_logger(), "ping_rate mode is seted to " << pingRateStandby << ".");
    disableRunMode();
  } else if (is_overheating_) {
    RCLCPP_FATAL_STREAM(this->get_logger(), "Temperature of sonar is to high ("
                                                << ping->temperature()
                                                << "°C). Make sur the sonar is underwatter. Security limit set at "
                                                << temperature_stop_limit_ << "°C");
    disableRunMode();
  } else if (ping->temperature() >= temperature_warn_limit_) {
    RCLCPP_WARN_STREAM(this->get_logger(), "Temperature of sonar is to high ("
                                               << ping->temperature()
                                               << "°C). Make sur the sonar is underwatter. Security limit set at "
                                               << temperature_stop_limit_ << "°C");
  }

  // Update current config with ping information
  currentSonarParameters_.frequency_mode = ping->master_mode();
  currentSonarParameters_.range = ping->range();
  currentSonarParameters_.gain_percent = ping->gain_percent();
  currentSonarParameters_.sound_speed = ping->speed_of_sound_used();
  updateRosConfig();

  static oculus_interfaces::msg::Ping msg;
  msg.header.frame_id = frame_id_;
  oculus::toMsg(msg, ping);
  this->ping_publisher_->publish(msg);

  sensor_msgs::msg::Temperature temperature_ros_msg;
  temperature_ros_msg.header = msg.header;
  temperature_ros_msg.temperature = msg.temperature;  // Measurement of the Temperature in Degrees Celsius
  temperature_ros_msg.variance = 0;  // 0 is interpreted as variance unknown
  this->temperature_publisher_->publish(temperature_ros_msg);

  sensor_msgs::msg::FluidPressure pressure_ros_msg;
  pressure_ros_msg.header = msg.header;
  pressure_ros_msg.fluid_pressure = msg.pressure;  // Absolute pressure reading in Pascals.
  pressure_ros_msg.variance = 0;  // 0 is interpreted as variance unknown
  this->pressure_publisher_->publish(pressure_ros_msg);

  // TODO(hugoyvrn, publish bearings)

  sonar_viewer_.publishFan(ping, frame_id_);
}

void OculusSonarNode::handleDummy() {
  if (is_running_ && get_subscription_count() > 0 && !is_overheating_ && currentSonarParameters_.ping_rate != pingRateStandby) {
    RCLCPP_INFO(this->get_logger(), "Exiting standby mode");
    enableRunMode();
  }
}

void OculusSonarNode::updateLocalParameters(SonarParameters& parameters, const std::vector<rclcpp::Parameter>& new_parameters) {
  for (const rclcpp::Parameter& new_param : new_parameters) {
    if (new_param.get_name() == params::FREQUENCY_MODE.name) {
      parameters.frequency_mode = new_param.as_int();
    } else if (new_param.get_name() == params::PING_RATE.name) {
      parameters.ping_rate = new_param.as_int();
    } else if (new_param.get_name() == params::NBEAMS.name) {
      parameters.nbeams = new_param.as_int();
    } else if (new_param.get_name() == params::GAIN_ASSIT.name) {
      parameters.gain_assist = new_param.as_bool();
    } else if (new_param.get_name() == params::RANGE.name) {
      parameters.range = new_param.as_double();
    } else if (new_param.get_name() == params::GAMMA_CORRECTION.name) {
      parameters.gamma_correction = new_param.as_int();
    } else if (new_param.get_name() == params::GAIN_PERCENT.name) {
      parameters.gain_percent = new_param.as_double();
    } else if (new_param.get_name() == params::SOUND_SPEED.name) {
      parameters.sound_speed = new_param.as_double();
    } else if (new_param.get_name() == params::USE_SALINITY.name) {
      parameters.use_salinity = new_param.as_bool();
    } else if (new_param.get_name() == params::SALINITY.name) {
      parameters.salinity = new_param.as_double();
    } else if (!(new_param.get_name() == "run")) {
      RCLCPP_WARN_STREAM(get_logger(), "Wrong parameter to set : new_param = " << new_param << ". Not seted");
    }
  }
}

void OculusSonarNode::updateLocalParameters(SonarParameters& parameters, SonarDriver::PingConfig feedback) {
  std::vector<rclcpp::Parameter> new_parameters;
  // OculusMessageHeader head;      // The standard message header
  // uint16_t oculusId;          // Fixed ID 0x4f53
  // uint16_t srcDeviceId;       // The device id of the source
  // uint16_t dstDeviceId;       // The device id of the destination
  // uint16_t msgId;             // Message identifier
  // uint16_t msgVersion;
  // uint32_t payloadSize;       // The size of the message payload (header not included)
  // uint16_t spare2;
  // uint8_t masterMode;            // mode 0 is flexi mode, needs full fire message (not available for third party developers)
  //                                // mode 1 - Low Frequency Mode (wide aperture, navigation)
  //                                // mode 2 - High Frequency Mode (narrow aperture, target identification)
  // uint8_t pingRate;              // Sets the maximum ping rate. was PingRateType
  // uint8_t networkSpeed;          // Used to reduce the network comms speed (useful for high latency shared links)
  // uint8_t gammaCorrection;       // 0 and 0xff = gamma correction = 1.0
  //                                // Set to 127 for gamma correction = 0.5
  // uint8_t flags;                 // bit 0: 0 = interpret range as percent, 1 = interpret range as meters
  //                                // bit 1: 0 = 8 bit data, 1 = 16 bit data  // inverted ?
  //                                // bit 2: 0 = won't send gain, 1 = send gain
  //                                // bit 3: 0 = send full return message, 1 = send simple return message
  //                                // bit 4: gain assist ? TODO
  //                                // bit 5: ? TODO
  //                                // bit 6: enable 512 beams
  //                                // bit 7: ? TODO
  // double range;                  // The range demand in percent or m depending on flags
  // double gainPercent;            // The gain demand
  // double speedOfSound;           // ms-1, if set to zero then internal calc will apply using salinity
  // double salinity;               // ppt, set to zero if we are in fresh water

  checkMinimalFlags(feedback.flags);

  new_parameters.push_back(rclcpp::Parameter(params::FREQUENCY_MODE.name, feedback.masterMode));
  new_parameters.push_back(rclcpp::Parameter(params::PING_RATE.name, feedback.pingRate));
  new_parameters.push_back(rclcpp::Parameter(params::NBEAMS.name, static_cast<int>(feedback.flags & flagByte::NBEAMS)));
  new_parameters.push_back(rclcpp::Parameter(params::GAIN_ASSIT.name, static_cast<bool>(feedback.flags & flagByte::GAIN_ASSIST)));
  new_parameters.push_back(rclcpp::Parameter(params::RANGE.name, feedback.range));
  new_parameters.push_back(rclcpp::Parameter(params::GAMMA_CORRECTION.name, feedback.gammaCorrection));
  new_parameters.push_back(rclcpp::Parameter(params::GAIN_PERCENT.name, feedback.gainPercent));
  new_parameters.push_back(rclcpp::Parameter(params::SOUND_SPEED.name, feedback.speedOfSound));
  //  // use_salinity  // TODO(hugoyvrn)
  // {
  //     rclcpp::Parameter param(params::USE_SALINITY.name, );
  //     new_parameters.push_back(param);
  // }

  new_parameters.push_back(rclcpp::Parameter(params::SALINITY.name, feedback.salinity));
  updateLocalParameters(parameters, new_parameters);
}

void OculusSonarNode::sendParamToSonar(rclcpp::Parameter param, rcl_interfaces::msg::SetParametersResult result) {
  SonarDriver::PingConfig newConfig = currentConfig_;  // To avoid to create a new SonarDriver::PingConfig from ros parameters
  if (param.get_name() == params::FREQUENCY_MODE.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating frequency_mode to " << param.as_int() << " (1: LowFreq, 2: HighFreq).");
    newConfig.masterMode = param.as_int();
  } else if (param.get_name() == params::PING_RATE.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating ping_rate to " << param.as_int() << " (" + params::PING_RATE.desc + ").");
    newConfig.pingRate = param.as_int();
  } else if (param.get_name() == params::NBEAMS.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating nbeams to " << param.as_int() << " (0: 256 beams, 1: 512 beams).");
    if (param.as_int() == 0) {
      newConfig.flags &= ~flagByte::NBEAMS;  // 256 beams
    } else {
      newConfig.flags |= flagByte::NBEAMS;  // 512 beams
    }
  } else if (param.get_name() == params::GAIN_ASSIT.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating gain_assist to " << param.as_bool());
    if (param.as_bool()) {
      newConfig.flags |= flagByte::GAIN_ASSIST;
    } else {
      newConfig.flags &= ~flagByte::GAIN_ASSIST;
    }
  } else if (param.get_name() == params::RANGE.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating range to " << param.as_double() << "m.");
    newConfig.range = param.as_double();
  } else if (param.get_name() == params::GAMMA_CORRECTION.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating gamma_correction to " << param.as_int());
    newConfig.gammaCorrection = param.as_int();
  } else if (param.get_name() == params::GAIN_PERCENT.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating gain_percent to " << param.as_double() << "%.");
    newConfig.gainPercent = param.as_double();
  } else if (param.get_name() == params::USE_SALINITY.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating use_salinity to " << param.as_bool());
    if (param.as_bool())
      newConfig.speedOfSound = 0.0;
  } else if (param.get_name() == params::SOUND_SPEED.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating sound_speed to " << param.as_double() << " m/s.");
    if (!currentRosParameters_.use_salinity) {
      if (param.as_double() >= 1400.0 &&
          param.as_double() <= 1600.0)  // TODO(hugoyvrn, why is there a range verification here and not for other parameters?)
        newConfig.speedOfSound = param.as_double();
      else
        RCLCPP_INFO_STREAM(this->get_logger(), "Speed of sound must be between 1400.0 and 1600.0.");
    }
  } else if (param.get_name() == params::SALINITY.name) {
    RCLCPP_INFO_STREAM(this->get_logger(), "Updating salinity to " << param.as_double() << " parts per thousand (ppt,ppm,g/kg).");
    newConfig.salinity = param.as_double();
  }

  setMinimalFlags(newConfig.flags);

  // send config to Oculus sonar and wait for feedback
  SonarDriver::PingConfig feedback = this->sonar_driver_->request_ping_config(newConfig);
  currentConfig_ = feedback;

  updateLocalParameters(currentSonarParameters_, feedback);

  checkMinimalFlags(feedback.flags);

  handleFeedbackForParam<double>(result, param, newConfig.masterMode, feedback.masterMode, params::FREQUENCY_MODE.name);
  // newConfig.pingRate      != feedback.pingRate  // is broken (?) sonar side TODO(???)
  handleFeedbackForParam<bool>(result, param, (newConfig.flags & flagByte::GAIN_ASSIST) ? 1 : 0,
      (feedback.flags & flagByte::GAIN_ASSIST) ? 1 : 0, params::GAIN_ASSIT.name);
  handleFeedbackForParam<int>(result, param, (newConfig.flags & flagByte::NBEAMS) ? 1 : 0,
      (feedback.flags & flagByte::NBEAMS) ? 1 : 0, params::NBEAMS.name);
  handleFeedbackForParam<double>(result, param, newConfig.range, feedback.range, params::RANGE.name);
  handleFeedbackForParam<int>(result, param, newConfig.gammaCorrection, feedback.gammaCorrection, params::GAMMA_CORRECTION.name);
  handleFeedbackForParam<double>(result, param, newConfig.gainPercent, feedback.gainPercent, params::GAIN_PERCENT.name);
  handleFeedbackForParam<double>(result, param, newConfig.speedOfSound, feedback.speedOfSound, params::SOUND_SPEED.name);
  handleFeedbackForParam<double>(result, param, newConfig.salinity, feedback.salinity, params::SALINITY.name);

  if (feedback.pingRate == pingRateStandby && is_running_) {
    is_running_ = false;  // Will do disableRunMode() next ping callback
  }
}

rcl_interfaces::msg::SetParametersResult OculusSonarNode::setConfigCallback(const std::vector<rclcpp::Parameter>& parameters) {
  std::shared_lock l(param_mutex_);

  if (parameters.size() != 1) {
    RCLCPP_WARN(get_logger(), "You should set parameters one by one.");
    RCLCPP_INFO_STREAM(get_logger(), "parameters = " << parameters);
    rcl_interfaces::msg::SetParametersResult result;
    result.successful = false;
    result.reason = "Parameters should be set one by one";
    return result;
  }

  rcl_interfaces::msg::SetParametersResult result;
  result.successful = true;
  result.reason = "";

  for (const rclcpp::Parameter& param : parameters) {
    if (param.get_name() == "run") {
      if (!is_running_ || param.as_bool()) {
        if (get_subscription_count() == 0 || is_overheating_ || currentSonarParameters_.ping_rate == pingRateStandby) {
          result.successful = false;
          result.reason = "The condition to go in run mode are not meeted.";
          if (get_subscription_count() == 0) {
            result.reason += " There is no subscriber nor to ping topic neither to image topic.";
          }
          if (is_overheating_) {
            result.reason +=
                " Temperature of sonar is to high."
                " Make sur the sonar is underwatter. Security limit set at " +
                std::to_string(temperature_stop_limit_) + "°C";
          }
          if (currentSonarParameters_.ping_rate == pingRateStandby) {
            result.reason += " ping_rate mode is seted to " + std::to_string(pingRateStandby) + ".";
          }
          return result;
        }
      }
      is_running_ = param.as_bool();

    } else if (std::find(dynamic_parameters_names_.begin(), dynamic_parameters_names_.end(), param.get_name()) !=
               dynamic_parameters_names_.end()) {
      // QUICK FIX TODO(hugoyvrn, gain_assist not working, to fix)
      if (currentSonarParameters_.gain_assist && currentSonarParameters_.frequency_mode &&
          param.get_name() == params::FREQUENCY_MODE.name) {
        result.reason = "You must set gain_assist to false before changing frequency TODO(to fix).";
        return result;
      }
      // END QUICK FIX
      sendParamToSonar(param, result);
    }
  }

  if (result.successful) {  // If the parameters will be updated to ros
    updateLocalParameters(currentRosParameters_, parameters);
  }

  return result;
}

int main(int argc, char* argv[]) {
  rclcpp::init(argc, argv);
  rclcpp::spin(std::make_shared<OculusSonarNode>());  // force to monothread
  rclcpp::shutdown();
  return 0;
}
