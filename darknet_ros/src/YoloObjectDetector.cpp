/*
 * YoloObjectDetector.cpp
 *
 *  Created on: Dec 19, 2016
 *      Author: Marko Bjelonic
 *   Institute: ETH Zurich, Robotic Systems Lab
 */

// yolo object detector
#include "darknet_ros/YoloObjectDetector.hpp"

// Check for xServer
#include <X11/Xlib.h>

#ifdef DARKNET_FILE_PATH
std::string darknetFilePath_ = DARKNET_FILE_PATH;
#else
#error Path of darknet repository is not defined in CMakeLists.txt.
#endif

namespace darknet_ros 
{

  char *cfg;
  char *weights;
  char *data;
  char **detectionNames;

  YoloObjectDetector::YoloObjectDetector(ros::NodeHandle nh)
      : nodeHandle_(nh),
        numClasses_(0),
        classLabels_(0),
        imageTransport_(nodeHandle_),
        rosBoxes_(0),
        rosBoxCounter_(0),

        // added by xzt:
        imagergb_sub(nodeHandle_,"/zed/zed_node/rgb_raw/image_raw_color",1),       //For depth inclussion
        pcldepth_sub(nodeHandle_,"/zed/zed_node/point_cloud/cloud_registered",1),   //For depth inclussion
        //laserscan_sub(nodeHandle_,"/scan",1),       //For depth inclussion: laser scan
        sync_1(MySyncPolicy_1(50), imagergb_sub, pcldepth_sub)        //For depth inclussion
        
  {
    ROS_INFO("[YoloObjectDetector] Node started.");

    // Read parameters from config file.
    if (!readParameters()) {
      ros::requestShutdown();
    }

    init();
  }

  YoloObjectDetector::~YoloObjectDetector()
  {
    {
      boost::unique_lock<boost::shared_mutex> lockNodeStatus(mutexNodeStatus_);
      isNodeRunning_ = false;
    }
    yoloThread_.join();
  }

  bool YoloObjectDetector::readParameters()
  {
    // Load common parameters.
    nodeHandle_.param("image_view/enable_opencv", viewImage_, true);
    nodeHandle_.param("image_view/wait_key_delay", waitKeyDelay_, 3);
    nodeHandle_.param("image_view/enable_console_output", enableConsoleOutput_, false);

    // Check if Xserver is running on Linux.
    if (XOpenDisplay(NULL)) {
      // Do nothing!
      ROS_INFO("[YoloObjectDetector] Xserver is running.");
    } else {
      ROS_INFO("[YoloObjectDetector] Xserver is not running.");
      viewImage_ = false;
    }

    // Set vector sizes.
    nodeHandle_.param("yolo_model/detection_classes/names", classLabels_,
                      std::vector<std::string>(0));
    numClasses_ = classLabels_.size();
    rosBoxes_ = std::vector<std::vector<RosBox_> >(numClasses_);
    rosBoxCounter_ = std::vector<int>(numClasses_);

    return true;
  }

  void YoloObjectDetector::init()
  {
    ROS_INFO("[YoloObjectDetector] init().");

    // Initialize deep network of darknet.
    std::string weightsPath;
    std::string configPath;
    std::string dataPath;
    std::string configModel;
    std::string weightsModel;

    // Threshold of object detection.
    float thresh;
    nodeHandle_.param("yolo_model/threshold/value", thresh, (float) 0.3);

    // Path to weights file.
    nodeHandle_.param("yolo_model/weight_file/name", weightsModel,
                      std::string("yolov3.weights"));
    nodeHandle_.param("weights_path", weightsPath, std::string("/default"));
    weightsPath += "/" + weightsModel;
    weights = new char[weightsPath.length() + 1];
    strcpy(weights, weightsPath.c_str());

    // Path to config file.
    nodeHandle_.param("yolo_model/config_file/name", configModel, std::string("yolov3.cfg"));
    nodeHandle_.param("config_path", configPath, std::string("/default"));
    configPath += "/" + configModel;
    cfg = new char[configPath.length() + 1];
    strcpy(cfg, configPath.c_str());

    // Path to data folder.
    dataPath = darknetFilePath_;
    dataPath += "/data";
    data = new char[dataPath.length() + 1];
    strcpy(data, dataPath.c_str());

    // Get classes.
    detectionNames = (char**) realloc((void*) detectionNames, (numClasses_ + 1) * sizeof(char*));
    for (int i = 0; i < numClasses_; i++) {
      detectionNames[i] = new char[classLabels_[i].length() + 1];
      strcpy(detectionNames[i], classLabels_[i].c_str());
    }

    // Load network.
    setupNetwork(cfg, weights, data, thresh, detectionNames, numClasses_,
                  0, 0, 1, 0.5, 0, 0, 0, 0);
    yoloThread_ = std::thread(&YoloObjectDetector::yolo, this);

    // Initialize publisher and subscriber.
    std::string cameraTopicName;
    int cameraQueueSize;
    std::string objectDetectorTopicName;
    int objectDetectorQueueSize;
    bool objectDetectorLatch;
    std::string boundingBoxesTopicName;
    int boundingBoxesQueueSize;
    bool boundingBoxesLatch;
    std::string detectionImageTopicName;
    int detectionImageQueueSize;
    bool detectionImageLatch;

    // added by xzt:
    std::string depthTopicName;        //For depth inclussion
    int depthQueueSize;                //For depth inclussion

    // laser scan:
    std::string scanTopicName;
    int scanQueueSize;

    nodeHandle_.param("subscribers/camera_reading/topic", cameraTopicName,
                      std::string("/zed/zed_node/rgb_raw/image_raw_color"));
    nodeHandle_.param("subscribers/camera_reading/queue_size", cameraQueueSize, 1);
    // depth:
    nodeHandle_.param("subscribers/camera_depth/topic", depthTopicName, std::string("/zed/zed_node/point_cloud/cloud_registered"));   //For depth inclussion
    nodeHandle_.param("subscribers/camera_depth/queue_size", depthQueueSize, 1);                            //For depth inclussion
    // scan:
    nodeHandle_.param("subscribers/laser_scan/topic", scanTopicName, std::string("/scan"));  // laser scan
    nodeHandle_.param("subscribers/laser_scan/queue_size", scanQueueSize, 1);

    nodeHandle_.param("publishers/object_detector/topic", objectDetectorTopicName,
                      std::string("found_object"));
    nodeHandle_.param("publishers/object_detector/queue_size", objectDetectorQueueSize, 1);
    nodeHandle_.param("publishers/object_detector/latch", objectDetectorLatch, false);
    nodeHandle_.param("publishers/bounding_boxes/topic", boundingBoxesTopicName,
                      std::string("bounding_boxes"));
    nodeHandle_.param("publishers/bounding_boxes/queue_size", boundingBoxesQueueSize, 1);
    nodeHandle_.param("publishers/bounding_boxes/latch", boundingBoxesLatch, false);
    nodeHandle_.param("publishers/detection_image/topic", detectionImageTopicName,
                      std::string("detection_image"));
    nodeHandle_.param("publishers/detection_image/queue_size", detectionImageQueueSize, 1);
    nodeHandle_.param("publishers/detection_image/latch", detectionImageLatch, true);

    sync_1.registerCallback(boost::bind(&YoloObjectDetector::cameraCallback,this,_1,_2));   //For depth inclussion


    //imageSubscriber_ = imageTransport_.subscribe(cameraTopicName, cameraQueueSize,
    //                                            &YoloObjectDetector::cameraCallback, this);
    objectPublisher_ = nodeHandle_.advertise<darknet_ros_msgs::ObjectCount>(objectDetectorTopicName,
                                                                              objectDetectorQueueSize,
                                                                              objectDetectorLatch);
    boundingBoxesPublisher_ = nodeHandle_.advertise<darknet_ros_msgs::BoundingBoxes>(
        boundingBoxesTopicName, boundingBoxesQueueSize, boundingBoxesLatch);
    detectionImagePublisher_ = nodeHandle_.advertise<sensor_msgs::Image>(detectionImageTopicName,
                                                                        detectionImageQueueSize,
                                                                        detectionImageLatch);

    // Action servers.
    std::string checkForObjectsActionName;
    nodeHandle_.param("actions/camera_reading/topic", checkForObjectsActionName,
                      std::string("check_for_objects"));
    checkForObjectsActionServer_.reset(
        new CheckForObjectsActionServer(nodeHandle_, checkForObjectsActionName, false));
    checkForObjectsActionServer_->registerGoalCallback(
        boost::bind(&YoloObjectDetector::checkForObjectsActionGoalCB, this));
    checkForObjectsActionServer_->registerPreemptCallback(
        boost::bind(&YoloObjectDetector::checkForObjectsActionPreemptCB, this));
    checkForObjectsActionServer_->start();
  }

  void YoloObjectDetector::cameraCallback(const sensor_msgs::ImageConstPtr& msg, const sensor_msgs::PointCloud2ConstPtr& msgdepth) //, const sensor_msgs::LaserScanConstPtr& scan_msg)
  {
    ROS_DEBUG("[YoloObjectDetector] USB image received.");
    // camera imgae:
    cv_bridge::CvImagePtr cam_image;

    try {
      cam_image = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
    } catch (cv_bridge::Exception& e) {
      ROS_ERROR("cv_bridge exception: %s", e.what());
      return;
    }

    if (cam_image) {
      {
        boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexImageCallback_);
        imageHeader_ = msg->header;
        camImageCopy_ = cam_image->image.clone();
      }
      {
        boost::unique_lock<boost::shared_mutex> lockImageStatus(mutexImageStatus_);
        imageStatus_ = true;
      }
      frameWidth_ = cam_image->image.size().width;
      frameHeight_ = cam_image->image.size().height;
    }

    // point cloud:
    pcl::fromROSMsg(*msgdepth, depth);

    // laser scan:
    //projector_.projectLaser(*scan_msg, cloud);
    // To get header data from sensor msg
    //scan_points = leg_detector(scan_msg);

    return;
  }

  void YoloObjectDetector::checkForObjectsActionGoalCB()
  {
    ROS_DEBUG("[YoloObjectDetector] Start check for objects action.");

    boost::shared_ptr<const darknet_ros_msgs::CheckForObjectsGoal> imageActionPtr =
        checkForObjectsActionServer_->acceptNewGoal();
    sensor_msgs::Image imageAction = imageActionPtr->image;

    cv_bridge::CvImagePtr cam_image;

    try {
      cam_image = cv_bridge::toCvCopy(imageAction, sensor_msgs::image_encodings::BGR8);
    } catch (cv_bridge::Exception& e) {
      ROS_ERROR("cv_bridge exception: %s", e.what());
      return;
    }

    if (cam_image) {
      {
        boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexImageCallback_);
        camImageCopy_ = cam_image->image.clone();
      }
      {
        boost::unique_lock<boost::shared_mutex> lockImageCallback(mutexActionStatus_);
        actionId_ = imageActionPtr->id;
      }
      {
        boost::unique_lock<boost::shared_mutex> lockImageStatus(mutexImageStatus_);
        imageStatus_ = true;
      }
      frameWidth_ = cam_image->image.size().width;
      frameHeight_ = cam_image->image.size().height;
    }
    return;
  }

  void YoloObjectDetector::checkForObjectsActionPreemptCB()
  {
    ROS_DEBUG("[YoloObjectDetector] Preempt check for objects action.");
    checkForObjectsActionServer_->setPreempted();
  }

  bool YoloObjectDetector::isCheckingForObjects() const
  {
    return (ros::ok() && checkForObjectsActionServer_->isActive()
        && !checkForObjectsActionServer_->isPreemptRequested());
  }

  bool YoloObjectDetector::publishDetectionImage(const cv::Mat& detectionImage)
  {
    if (detectionImagePublisher_.getNumSubscribers() < 1)
      return false;
    cv_bridge::CvImage cvImage;
    cvImage.header.stamp = ros::Time::now();
    cvImage.header.frame_id = "detection_image";
    cvImage.encoding = sensor_msgs::image_encodings::BGR8;
    cvImage.image = detectionImage;
    detectionImagePublisher_.publish(*cvImage.toImageMsg());
    ROS_DEBUG("Detection image has been published.");
    return true;
  }

  // double YoloObjectDetector::getWallTime()
  // {
  //   struct timeval time;
  //   if (gettimeofday(&time, NULL)) {
  //     return 0;
  //   }
  //   return (double) time.tv_sec + (double) time.tv_usec * .000001;
  // }

  int YoloObjectDetector::sizeNetwork(network *net)
  {
    int i;
    int count = 0;
    for(i = 0; i < net->n; ++i){
      layer l = net->layers[i];
      if(l.type == YOLO || l.type == REGION || l.type == DETECTION){
        count += l.outputs;
      }
    }
    return count;
  }

  void YoloObjectDetector::rememberNetwork(network *net)
  {
    int i;
    int count = 0;
    for(i = 0; i < net->n; ++i){
      layer l = net->layers[i];
      if(l.type == YOLO || l.type == REGION || l.type == DETECTION){
        memcpy(predictions_[demoIndex_] + count, net->layers[i].output, sizeof(float) * l.outputs);
        count += l.outputs;
      }
    }
  }

  detection *YoloObjectDetector::avgPredictions(network *net, int *nboxes)
  {
    int i, j;
    int count = 0;
    fill_cpu(demoTotal_, 0, avg_, 1);
    for(j = 0; j < demoFrame_; ++j){
      axpy_cpu(demoTotal_, 1./demoFrame_, predictions_[j], 1, avg_, 1);
    }
    for(i = 0; i < net->n; ++i){
      layer l = net->layers[i];
      if(l.type == YOLO || l.type == REGION || l.type == DETECTION){
        memcpy(l.output, avg_ + count, sizeof(float) * l.outputs);
        count += l.outputs;
      }
    }
    detection *dets = get_network_boxes(net, buff_[0].w, buff_[0].h, demoThresh_, demoHier_, 0, 1, nboxes);
    return dets;
  }

  void *YoloObjectDetector::detectInThread()
  {
    running_ = 1;
    float nms = .4;

    layer l = net_->layers[net_->n - 1];
    float *X = buffLetter_[(buffIndex_ + 2) % 3].data;
    float *prediction = network_predict(net_, X);

    rememberNetwork(net_);
    detection *dets = 0;
    int nboxes = 0;
    dets = avgPredictions(net_, &nboxes);

    if (nms > 0) do_nms_obj(dets, nboxes, l.classes, nms);

    if (enableConsoleOutput_) {
      printf("\033[2J");
      printf("\033[1;1H");
      printf("\nFPS:%.1f\n",fps_);
      printf("Objects:\n\n");
    }
    image display = buff_[(buffIndex_+2) % 3];
    draw_detections(display, dets, nboxes, demoThresh_, demoNames_, demoAlphabet_, demoClasses_);

    // extract the bounding boxes and send them to ROS
    int i, j;
    int count = 0;
    for (i = 0; i < nboxes; ++i) {
      float xmin = dets[i].bbox.x - dets[i].bbox.w / 2.;
      float xmax = dets[i].bbox.x + dets[i].bbox.w / 2.;
      float ymin = dets[i].bbox.y - dets[i].bbox.h / 2.;
      float ymax = dets[i].bbox.y + dets[i].bbox.h / 2.;

      if (xmin < 0)
        xmin = 0;
      if (ymin < 0)
        ymin = 0;
      if (xmax > 1)
        xmax = 1;
      if (ymax > 1)
        ymax = 1;

      // iterate through possible boxes and collect the bounding boxes
      for (j = 0; j < demoClasses_; ++j) {
        if (dets[i].prob[j]) {
          float x_center = (xmin + xmax) / 2;
          float y_center = (ymin + ymax) / 2;
          float BoundingBox_width = xmax - xmin;
          float BoundingBox_height = ymax - ymin;

          // define bounding box
          // BoundingBox must be 1% size of frame (3.2x2.4 pixels)
          if (BoundingBox_width > 0.01 && BoundingBox_height > 0.01) {
            roiBoxes_[count].x = x_center;
            roiBoxes_[count].y = y_center;
            roiBoxes_[count].w = BoundingBox_width;
            roiBoxes_[count].h = BoundingBox_height;
            roiBoxes_[count].Class = j;
            roiBoxes_[count].prob = dets[i].prob[j];
            count++;
          }
        }
      }
    }

    // create array to store found bounding boxes
    // if no object detected, make sure that ROS knows that num = 0
    if (count == 0) {
      roiBoxes_[0].num = 0;
    } else {
      roiBoxes_[0].num = count;
    }

    free_detections(dets, nboxes);
    demoIndex_ = (demoIndex_ + 1) % demoFrame_;
    running_ = 0;
    return 0;
  }

  void *YoloObjectDetector::fetchInThread()
  {
    {
      boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
      MatWithHeader_ imageAndHeader = getIplImageWithHeader();
      cv::Mat ROS_img = imageAndHeader.image;
      mat_to_image(ROS_img, buff_ + buffIndex_);
      headerBuff_[buffIndex_] = imageAndHeader.header;
      buffId_[buffIndex_] = actionId_;
    }
    rgbgr_image(buff_[buffIndex_]);
    letterbox_image_into(buff_[buffIndex_], net_->w, net_->h, buffLetter_[buffIndex_]);
    return 0;
  }

  void *YoloObjectDetector::displayInThread(void *ptr)
  {
    int c = show_image(buff_[(buffIndex_ + 1)%3], "YOLO V3", waitKeyDelay_);
    /*
      // record detection video: add by xzt
      cv::Mat pic  = cv::cvarrToMat(ipl_);
      if(init_flag == 1)
      {
         Size pic_size = pic.size();
         writer.open("./yolo_demo.avi", CV_FOURCC('M', 'J', 'P', 'G'), 25, pic_size, true);
         init_flag = 0;
      }
      writer.write(pic);
      // video end
      */
    if (c != -1) c = c%256;
    if (c == 27) {
        demoDone_ = 1;
        return 0;
    } else if (c == 82) {
        demoThresh_ += .02;
    } else if (c == 84) {
        demoThresh_ -= .02;
        if(demoThresh_ <= .02) demoThresh_ = .02;
    } else if (c == 83) {
        demoHier_ += .02;
    } else if (c == 81) {
        demoHier_ -= .02;
        if(demoHier_ <= .0) demoHier_ = .0;
    }
    return 0;
  }

  void *YoloObjectDetector::displayLoop(void *ptr)
  {
    while (1) {
      displayInThread(0);
    }
  }

  void *YoloObjectDetector::detectLoop(void *ptr)
  {
    while (1) {
      detectInThread();
    }
  }

  void YoloObjectDetector::setupNetwork(char *cfgfile, char *weightfile, char *datafile, float thresh,
                                        char **names, int classes,
                                        int delay, char *prefix, int avg_frames, float hier, int w, int h,
                                        int frames, int fullscreen)
  {
    demoPrefix_ = prefix;
    demoDelay_ = delay;
    demoFrame_ = avg_frames;
    image **alphabet = load_alphabet_with_file(datafile);
    demoNames_ = names;
    demoAlphabet_ = alphabet;
    demoClasses_ = classes;
    demoThresh_ = thresh;
    demoHier_ = hier;
    fullScreen_ = fullscreen;
    printf("YOLO V3\n");
    net_ = load_network(cfgfile, weightfile, 0);
    set_batch_network(net_, 1);
  }

  void YoloObjectDetector::yolo()
  {
    const auto wait_duration = std::chrono::milliseconds(2000);
    while (!getImageStatus()) {
      printf("Waiting for image.\n");
      if (!isNodeRunning()) {
        return;
      }
      std::this_thread::sleep_for(wait_duration);
    }

    std::thread detect_thread;
    std::thread fetch_thread;

    srand(2222222);

    int i;
    demoTotal_ = sizeNetwork(net_);
    predictions_ = (float **) calloc(demoFrame_, sizeof(float*));
    for (i = 0; i < demoFrame_; ++i){
        predictions_[i] = (float *) calloc(demoTotal_, sizeof(float));
    }
    avg_ = (float *) calloc(demoTotal_, sizeof(float));

    layer l = net_->layers[net_->n - 1];
    roiBoxes_ = (darknet_ros::RosBox_ *) calloc(l.w * l.h * l.n, sizeof(darknet_ros::RosBox_));

    {
      boost::shared_lock<boost::shared_mutex> lock(mutexImageCallback_);
      MatWithHeader_ imageAndHeader = getIplImageWithHeader();
      cv::Mat ROS_img = imageAndHeader.image;
      buff_[0] = mat_to_image(ROS_img);
      headerBuff_[0] = imageAndHeader.header;
    }
    buff_[1] = copy_image(buff_[0]);
    buff_[2] = copy_image(buff_[0]);
    headerBuff_[1] = headerBuff_[0];
    headerBuff_[2] = headerBuff_[0];
    buffLetter_[0] = letterbox_image(buff_[0], net_->w, net_->h);
    buffLetter_[1] = letterbox_image(buff_[0], net_->w, net_->h);
    buffLetter_[2] = letterbox_image(buff_[0], net_->w, net_->h);
    ipl_ = cv::Mat(buff_[0].h, buff_[0].w,
            CV_8UC(buff_[0].c));

    int count = 0;

    if (!demoPrefix_ && viewImage_) {
        cv::namedWindow("YOLO V3", cv::WINDOW_NORMAL);
      if (fullScreen_) {
        cv::setWindowProperty("YOLO V3", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);
      } else {
        cv::moveWindow("YOLO V3", 0, 0);
        cv::resizeWindow("YOLO V3", 640, 480);
      }
    }

    demoTime_ = what_time_is_it_now();

    while (!demoDone_) {
      buffIndex_ = (buffIndex_ + 1) % 3;
      fetch_thread = std::thread(&YoloObjectDetector::fetchInThread, this);
      detect_thread = std::thread(&YoloObjectDetector::detectInThread, this);
      if (!demoPrefix_) {
        fps_ = 1./(what_time_is_it_now() - demoTime_);
        demoTime_ = what_time_is_it_now();
        if (viewImage_) {
          // added by xzt: use to display on rviz
          generate_image(buff_[(buffIndex_ + 1)%3], ipl_);
          displayInThread(0);
        } else {
          generate_image(buff_[(buffIndex_ + 1)%3], ipl_);
        }
        publishInThread();
      } else {
        char name[256];
        sprintf(name, "%s_%08d", demoPrefix_, count);
        save_image(buff_[(buffIndex_ + 1) % 3], name);
      }
      fetch_thread.join();
      detect_thread.join();
      ++count;
      if (!isNodeRunning()) {
        demoDone_ = true;
      }
    }

  }

  MatWithHeader_ YoloObjectDetector::getIplImageWithHeader()
  {
    MatWithHeader_ header {camImageCopy_, imageHeader_};
    return header;
  }

  bool YoloObjectDetector::getImageStatus(void)
  {
    boost::shared_lock<boost::shared_mutex> lock(mutexImageStatus_);
    return imageStatus_;
  }

  bool YoloObjectDetector::isNodeRunning(void)
  {
    boost::shared_lock<boost::shared_mutex> lock(mutexNodeStatus_);
    return isNodeRunning_;
  }

  void *YoloObjectDetector::publishInThread()
  {
    // Publish image.
    cv::Mat cvImage = ipl_;
    //changed by xzt: publish the image with position information
    cv::Mat pic;
    /* 
    if (!publishDetectionImage(cv::Mat(cvImage))) {
      ROS_DEBUG("Detection image has not been broadcasted.");
    }
    */

    // Publish bounding boxes and detection result.
    int num = roiBoxes_[0].num;
    if (num > 0 && num <= 100) {
      for (int i = 0; i < num; i++) {
        for (int j = 0; j < numClasses_; j++) {
          if (roiBoxes_[i].Class == j) {
            rosBoxes_[j].push_back(roiBoxes_[i]);
            rosBoxCounter_[j]++;
          }
        }
      }

      darknet_ros_msgs::ObjectCount msg;
      msg.header.stamp = ros::Time::now();
      msg.header.frame_id = "detection";
      msg.count = num;
      objectPublisher_.publish(msg);

      for (int i = 0; i < numClasses_; i++) {
        if (rosBoxCounter_[i] > 0) {
          darknet_ros_msgs::BoundingBox boundingBox;

          for (int j = 0; j < rosBoxCounter_[i]; j++) {
            int xmin = (rosBoxes_[i][j].x - rosBoxes_[i][j].w / 2) * frameWidth_;
            int ymin = (rosBoxes_[i][j].y - rosBoxes_[i][j].h / 2) * frameHeight_;
            int xmax = (rosBoxes_[i][j].x + rosBoxes_[i][j].w / 2) * frameWidth_;
            int ymax = (rosBoxes_[i][j].y + rosBoxes_[i][j].h / 2) * frameHeight_;

            // added by xzt:
            pic = YoloObjectDetector::Coordinates(xmin, ymin, xmax, ymax);

            boundingBox.Class = classLabels_[i];
            boundingBox.id = i;
            boundingBox.probability = rosBoxes_[i][j].prob;
            boundingBox.xmin = xmin;
            boundingBox.ymin = ymin;
            boundingBox.xmax = xmax;
            boundingBox.ymax = ymax;

            boundingBox.X_C = X_C;
            boundingBox.Y_C = Y_C;
            boundingBox.Z_C = Z_C;
            /*
            boundingBox.X_L = X_L;
            boundingBox.Y_L = Y_L;
            boundingBox.Z_L = Z_L;
            */
            boundingBoxesResults_.bounding_boxes.push_back(boundingBox);
            //printf("%s, X %f, Y %f, Z %f \n", classLabels_[i], X, Y, Z);
          }
        }
      }
      boundingBoxesResults_.header.stamp = imageHeader_.stamp; //ros::Time::now();
      boundingBoxesResults_.header.frame_id = "detection";
      boundingBoxesResults_.image_header = headerBuff_[(buffIndex_ + 1) % 3];
      boundingBoxesPublisher_.publish(boundingBoxesResults_);
    } else {
      darknet_ros_msgs::ObjectCount msg;
      msg.header.stamp = ros::Time::now();
      msg.header.frame_id = "detection";
      msg.count = 0;
      objectPublisher_.publish(msg);

      // add by xzt:
      darknet_ros_msgs::BoundingBox boundingBox;
      boundingBox.Class = "None";
      boundingBox.probability = 0;
      boundingBox.xmin = 0;
      boundingBox.ymin = 0;
      boundingBox.xmax = 0;
      boundingBox.ymax = 0;
      boundingBox.X_C = 0;
      boundingBox.Y_C = 0;
      boundingBox.Z_C = 0;
      /*
      boundingBox.X_L = 0;
      boundingBox.Y_L = 0;
      boundingBox.Z_L = 0;
      */
      boundingBoxesResults_.bounding_boxes.push_back(boundingBox);     

      boundingBoxesResults_.header.stamp = imageHeader_.stamp; //ros::Time::now();
      boundingBoxesResults_.header.frame_id = "detection";
      boundingBoxesResults_.image_header = imageHeader_;
      boundingBoxesPublisher_.publish(boundingBoxesResults_); 
    }

    // added by xzt:
    if (!publishDetectionImage(cv::Mat(pic)))
    {
        ROS_DEBUG("Detection image has not been broadcasted.");
    }

    if (isCheckingForObjects()) {
      ROS_DEBUG("[YoloObjectDetector] check for objects in image.");
      darknet_ros_msgs::CheckForObjectsResult objectsActionResult;
      objectsActionResult.id = buffId_[0];
      objectsActionResult.bounding_boxes = boundingBoxesResults_;
      checkForObjectsActionServer_->setSucceeded(objectsActionResult, "Send bounding boxes.");
    }
    boundingBoxesResults_.bounding_boxes.clear();
    for (int i = 0; i < numClasses_; i++) {
      rosBoxes_[i].clear();
      rosBoxCounter_[i] = 0;
    }

    /* 
    // store detction image: by xzt
    // imshow("pic", pic);
    char img_name[60];
    sprintf(img_name, "%s%d%s", "/home/xzt/detection_data/", laser_num++, ".jpg");
    if(laser_num%30 == 1)
    {
        //imwrite("/home/xzt/laser.jpg", pic);
        imwrite(img_name, pic);
        // cv::waitKey(0);
    }*/

    return 0;
  }

  // added by xzt:
  // get the coordinates of objects:
  cv::Mat YoloObjectDetector::Coordinates(int xmin, int ymin, int xmax, int ymax)
  {
    int Xcenter = ((xmax-xmin)/2) + xmin;
    int Ycenter = ((ymax-ymin)/2) + ymin;

    int Xleft = ((xmax-xmin)/4) + xmin;
    int Yleft = ((ymax-ymin)/4) + ymin;

    int Xright = (3*(xmax-xmin)/4) + xmin;
    int Yright = (3*(ymax-ymin)/4) + ymin;

    cv::Mat pic  = ipl_; //cv::cvarrToMat(ipl_); //= camImageCopy_.clone();

    // zed camera: center position:
    float x_c_c = 0, y_c_c = 0, z_c_c = 0;
    int cnt_c = 0;
    for(int i = Xcenter-1; i <= Xcenter+1; i++)
    {
        for(int j = Ycenter-1; j <= Ycenter+1; j++)
        {
          pcl::PointXYZ pos = depth.at(i, j);

          if(std::isinf(pos.z)==1 || std::isnan(pos.z)==1) 
          {
              x_c_c = 0;
              y_c_c = 0;
              z_c_c = 0;
          }
          else
          {
              x_c_c = x_c_c + pos.x;         
              y_c_c = y_c_c + pos.y;          
              z_c_c = z_c_c + pos.z; 
              cnt_c++;
          }
        }
    }    
    // zed camera: left position:
    float x_c_l = 0, y_c_l = 0, z_c_l = 0;
    int cnt_l = 0;
    for(int i = Xleft-1; i <= Xleft+1; i++)
    {
        for(int j = Yleft-1; j <= Yleft+1; j++)
        {
          pcl::PointXYZ pos = depth.at(i, j);

          if(std::isinf(pos.z)==1 || std::isnan(pos.z)==1) 
          {
              x_c_l = 0;
              y_c_l = 0;
              z_c_l = 0;
          }
          else
          {
              x_c_l = x_c_l + pos.x;         
              y_c_l = y_c_l + pos.y;          
              z_c_l = z_c_l + pos.z; 
              cnt_l++;
          }
        }
    }   
    // zed camera: right position:
    float x_c_r = 0, y_c_r = 0, z_c_r = 0;
    int cnt_r = 0;
    for(int i = Xright-1; i <= Xright+1; i++)
    {
        for(int j = Yright-1; j <= Yright+1; j++)
        {
          pcl::PointXYZ pos = depth.at(i, j);

          if(std::isinf(pos.z)==1 || std::isnan(pos.z)==1) 
          {
              x_c_r = 0;
              y_c_r = 0;
              z_c_r = 0;
          }
          else
          {
              x_c_r = x_c_r + pos.x;         
              y_c_r = y_c_r + pos.y;          
              z_c_r = z_c_r + pos.z; 
              cnt_r++;
          }
        }
    }  

    // find the minimum distance value:
    float dist_c = x_c_c*x_c_c + y_c_c*y_c_c;
    float dist_l = x_c_l*x_c_l + y_c_l*y_c_l;
    float dist_r = x_c_r*x_c_r + y_c_r*y_c_r;
    int cnt;
    float x_c, y_c, z_c;
    if((dist_l <= dist_r) && (dist_l <= dist_c))
    {
        cnt = cnt_l;
        x_c = x_c_l;
        y_c = y_c_l;
        z_c = z_c_l;
    }
    else if((dist_r <= dist_l) && (dist_r <= dist_c))
    {
        cnt = cnt_r;
        x_c = x_c_r;
        y_c = y_c_r;
        z_c = z_c_r;
    }
    else
    {
        cnt = cnt_c;
        x_c = x_c_c;
        y_c = y_c_c;
        z_c = z_c_c;
    }
    

    // mean filer:
    if(cnt == 0)
    {
        X_C = 0;
        Y_C = 0;
        Z_C = 0;
    }
    else
    {
        // depth frame in gazebo is set to the optical coordinate frame (z forward) -> ROS coordinate frame (x forward) 
        X_C = z_c/cnt + 0.1; //
        Y_C = -x_c/cnt + 0.0125;  //
        Z_C = -y_c/cnt + 0.46;  //
        
        /*/ calibration position:
        X_C = 1.109*(x_c/cnt) - 0.04482;
        Y_C = 1.045*(y_c/cnt) + 0.07267;
        Z_C = z_c/cnt;  /*/
    }

    // draw position results:
    cv::Point p;
    p.x = Xcenter;
    p.y = Ycenter;
    cv::circle(pic, p, 4, cv::Scalar(0,0,255), 1.5);

    std::stringstream str_x0, str_y0, str_z0;
    std::string str_x,str_y, str_z;
    

    str_x0 << fixed << setprecision(2)<<X_C;
    str_y0 << fixed << setprecision(2)<<Y_C;
    str_z0 << fixed << setprecision(2)<<Z_C;

    str_x0 >> str_x;
    str_y0 >> str_y;
    str_z0 >> str_z;
  
    std::string str_l = "(" + str_x + "," + str_y  + "," + str_z + ")";
    cv::putText(pic, str_l, p, cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 0, 255), 2.5);
    
    //IplImage ipltemp = pic;
    //cvCopy(&ipltemp, ipl_);
    return pic;   
  }


} /* namespace darknet_ros*/
