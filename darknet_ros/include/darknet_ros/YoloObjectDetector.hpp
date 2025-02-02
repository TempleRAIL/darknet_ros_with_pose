/*
 * YoloObjectDetector.h
 *
 *  Created on: Dec 19, 2016
 *      Author: Marko Bjelonic
 *   Institute: ETH Zurich, Robotic Systems Lab
 */

#pragma once

// c++
#include <math.h>
#include <string>
#include <vector>
#include <iostream>
#include <pthread.h>
#include <thread>
#include <chrono>

#include <stdio.h>     //For depth inclussion

// ROS
#include <ros/ros.h>
#include <std_msgs/Header.h>
#include <actionlib/server/simple_action_server.h>
#include <sensor_msgs/image_encodings.h>
#include <sensor_msgs/Image.h>
#include <geometry_msgs/Point.h>
#include <image_transport/image_transport.h>

#include <std_msgs/String.h>                         //For depth inclussion
#include <message_filters/subscriber.h>                       //For depth inclussion
#include <message_filters/synchronizer.h>                     //For depth inclussion
#include <message_filters/time_synchronizer.h>                //For depth inclussion
#include <message_filters/sync_policies/approximate_time.h>   //For depth inclussion
#include <image_transport/subscriber_filter.h>                //For depth inclussion
#include <tf/transform_listener.h>

// point cloud: For depth inclussion
#include <pcl/point_types.h>
#include <pcl/PCLPointCloud2.h>
#include <sensor_msgs/PointCloud2.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/point_cloud.h>

// Laser scan:
#include <sensor_msgs/LaserScan.h>
/*
#include <laser_geometry/laser_geometry.h>                    //For laser image projection
// #include <Eigen/Dense>                                        //For laser image projection
#include "darknet_ros/leg_detector.h"
*/

// OpenCv
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/objdetect/objdetect.hpp>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/core/utility.hpp>          //For depth inclussion

// darknet_ros_msgs
#include <darknet_ros_msgs/BoundingBoxes.h>
#include <darknet_ros_msgs/BoundingBox.h>
#include <darknet_ros_msgs/ObjectCount.h>
#include <darknet_ros_msgs/CheckForObjectsAction.h>

// Darknet.
#ifdef GPU
#include "cuda_runtime.h"
#include "curand.h"
#include "cublas_v2.h"
#endif

#include "network.h"
#include "detection_layer.h"
#include "region_layer.h"
#include "cost_layer.h"
#include "utils.h"
#include "parser.h"
#include "image.h"
#include "box.h"
#include "darknet_ros/image_interface.h"
#include <sys/time.h>

using namespace std;
using namespace cv;

namespace darknet_ros {

//! Bounding box of the detected object.
typedef struct
{
  float x, y, w, h, prob;
  int num, Class;
} RosBox_;

struct MatWithHeader_ {
    cv::Mat image;
    std_msgs::Header header;

    MatWithHeader_() = default;
    MatWithHeader_(cv::Mat img, std_msgs::Header hdr):
        image(img.clone()), header(hdr) {}
};


class YoloObjectDetector
{
 public:
  /*!
   * Constructor.
   */
  explicit YoloObjectDetector(ros::NodeHandle nh);

  /*!
   * Destructor.
   */
  ~YoloObjectDetector();

 private:
  /*!
   * Reads and verifies the ROS parameters.
   * @return true if successful.
   */
  bool readParameters();

  /*!
   * Initialize the ROS connections.
   */
  void init();

  /*!
   * Callback of camera.
   * @param[in] msg image pointer.
   */
  // Callback of camera - @param[in] msg image pointer.
  // changed by xzt:
  void cameraCallback(const sensor_msgs::ImageConstPtr& msg, const sensor_msgs::PointCloud2ConstPtr& msgdepth); //, const sensor_msgs::LaserScanConstPtr& scan_msg);

  /*!
   * Check for objects action goal callback.
   */
  void checkForObjectsActionGoalCB();

  /*!
   * Check for objects action preempt callback.
   */
  void checkForObjectsActionPreemptCB();

  /*!
   * Check if a preempt for the check for objects action has been requested.
   * @return false if preempt has been requested or inactive.
   */
  bool isCheckingForObjects() const;

  /*!
   * Publishes the detection image.
   * @return true if successful.
   */
  bool publishDetectionImage(const cv::Mat& detectionImage);

  //! Typedefs.
  typedef actionlib::SimpleActionServer<darknet_ros_msgs::CheckForObjectsAction> CheckForObjectsActionServer;
  typedef std::shared_ptr<CheckForObjectsActionServer> CheckForObjectsActionServerPtr;

  //! ROS node handle.
  ros::NodeHandle nodeHandle_;

  //! Class labels.
  int numClasses_;
  std::vector<std::string> classLabels_;

  //! Check for objects action server.
  CheckForObjectsActionServerPtr checkForObjectsActionServer_;

  //! Advertise and subscribe to image topics.
  image_transport::ImageTransport imageTransport_;

  //! ROS subscriber and publisher.
  image_transport::Subscriber imageSubscriber_;
  ros::Publisher objectPublisher_;
  ros::Publisher boundingBoxesPublisher_;

  //! added by xzt:
  // Syncronizing Image messages - For depth inclussion
  message_filters::Subscriber<sensor_msgs::Image> imagergb_sub; 
  message_filters::Subscriber<sensor_msgs::PointCloud2> pcldepth_sub; 
  // message_filters::Subscriber<sensor_msgs::LaserScan> laserscan_sub;   // Laser Scan 
  typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::PointCloud2> MySyncPolicy_1;
  message_filters::Synchronizer<MySyncPolicy_1> sync_1;

  /*
  // laser image projection:
  vector<Points4d> Laser2Points();
  cv::Mat K;
  cv::Mat D;
  cv::Mat Rcl;
  cv::Mat tcl;
  
  // laser scan:
  vector < geometry_msgs::Point > scan_points;
  */

  // Depth Image - For depth inclussion
  // Create a container for the data.
  pcl::PointCloud<pcl::PointXYZ> depth;
  cv::Mat Coordinates(int xmin, int ymin, int xmax, int ymax);
  float X_C;
  float Y_C;
  float Z_C;
  /*
  float X_L;
  float Y_L;
  float Z_L;
  */
  int laser_num = 0;
  // vide record:
  //CvVideoWriter writer;
  //int init_flag = 1;  // initial video recorder
  

  //! Detected objects.
  std::vector<std::vector<RosBox_> > rosBoxes_;
  std::vector<int> rosBoxCounter_;
  darknet_ros_msgs::BoundingBoxes boundingBoxesResults_;

  //! Camera related parameters.
  int frameWidth_;
  int frameHeight_;

  //! Publisher of the bounding box image.
  ros::Publisher detectionImagePublisher_;

  // Yolo running on thread.
  std::thread yoloThread_;

  // Darknet.
  char **demoNames_;
  image **demoAlphabet_;
  int demoClasses_;

  network *net_;
  std_msgs::Header headerBuff_[3];
  image buff_[3];
  image buffLetter_[3];
  int buffId_[3];
  int buffIndex_ = 0;
  cv::Mat ipl_;
  float fps_ = 0;
  float demoThresh_ = 0;
  float demoHier_ = .5;
  int running_ = 0;

  int demoDelay_ = 0;
  int demoFrame_ = 3;
  float **predictions_;
  int demoIndex_ = 0;
  int demoDone_ = 0;
  float *lastAvg2_;
  float *lastAvg_;
  float *avg_;
  int demoTotal_ = 0;
  double demoTime_;

  RosBox_ *roiBoxes_;
  bool viewImage_;
  bool enableConsoleOutput_;
  int waitKeyDelay_;
  int fullScreen_;
  char *demoPrefix_;

  std_msgs::Header imageHeader_;
  cv::Mat camImageCopy_;
  boost::shared_mutex mutexImageCallback_;

  bool imageStatus_ = false;
  boost::shared_mutex mutexImageStatus_;

  bool isNodeRunning_ = true;
  boost::shared_mutex mutexNodeStatus_;

  int actionId_;
  boost::shared_mutex mutexActionStatus_;

  // double getWallTime();

  int sizeNetwork(network *net);

  void rememberNetwork(network *net);

  detection *avgPredictions(network *net, int *nboxes);

  void *detectInThread();

  void *fetchInThread();

  void *displayInThread(void *ptr);

  void *displayLoop(void *ptr);

  void *detectLoop(void *ptr);

  void setupNetwork(char *cfgfile, char *weightfile, char *datafile, float thresh,
                    char **names, int classes,
                    int delay, char *prefix, int avg_frames, float hier, int w, int h,
                    int frames, int fullscreen);

  void yolo();

  MatWithHeader_ getIplImageWithHeader();

  bool getImageStatus(void);

  bool isNodeRunning(void);

  void *publishInThread();
};

} /* namespace darknet_ros*/
