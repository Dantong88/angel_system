#include <chrono>
#include <cmath>
#include <exception>
#include <memory>
#include <numeric>

// ROS2 things
#include <builtin_interfaces/msg/time.hpp>
#include <cv_bridge/cv_bridge.h>
#include <rcl_interfaces/msg/parameter_descriptor.hpp>
#include <rcl_interfaces/msg/parameter_type.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_components/register_node_macro.hpp>
#include <sensor_msgs/msg/image.hpp>

// Other stuff
#include <opencv2/opencv.hpp>

// Our stuff
#include <angel_msgs/msg/object_detection2d_set.hpp>

using angel_msgs::msg::ObjectDetection2dSet;
using rcl_interfaces::msg::ParameterDescriptor;
using rcl_interfaces::msg::ParameterType;
using std::placeholders::_1;

namespace angel_debug {

namespace {

// ----------------------------------------------------------------------------
#define DEFINE_PARAM_NAME( var_name, param_name_str ) \
  static constexpr char const* var_name = param_name_str

// Topic we expect to receive headset RGB images from.
DEFINE_PARAM_NAME( PARAM_TOPIC_INPUT_IMAGES, "topic_input_images" );
// Topic we expect to receive 2D detections from.
DEFINE_PARAM_NAME( PARAM_TOPIC_INPUT_DET_2D, "topic_input_det_2d" );
// Topic we will output debug overlay images to. This needs to be a compressed
// image-transport topic.
DEFINE_PARAM_NAME( PARAM_TOPIC_OUTPUT_IMAGE, "topic_output_images" );
// Maximum number of images we will retain to map new detections onto, so that
// we do not continue to collect the world. This is in raw frames, so adjust
// accordingly based on frame-rate.
DEFINE_PARAM_NAME( PARAM_MAX_IMAGE_HISTORY, "max_image_history" );

#undef DEFINE_PARAM_NAME

// Nano-scale
static constexpr size_t const TENe9 = 1000000000;
// Factor of image max dim to determine line thickness, round result.
static constexpr double const LINE_FACTOR = 0.0015;

/// Convert a header instance into a single-value time component to be used as
/// and order-able key.
constexpr
size_t
time_key_from_header( builtin_interfaces::msg::Time const& header )
{
  return ( header.sec * TENe9 ) + header.nanosec;
}

/// Deduce a line thickness to use for drawing shapes on a given matrix.
int
thickness_for_drawing( cv::Mat img_mat )
{
  auto max_dim = std::max( img_mat.rows, img_mat.cols );
  return round( max_dim * LINE_FACTOR );
}

} // namespace

/// Keep track of the rate at which something is cycling.
/// TODO: Move to general utils package.
class RateTracker
{
public:
  RateTracker( size_t window_size = 10 )
    : m_window_size( window_size )
  {}

  virtual ~RateTracker() = default;

  /// Perform a measurement of the time since the last tick.
  void
  tick()
  {
    auto pub_time = clock_t::now();
    if( m_first_tick )
    {
      m_first_tick = false;
    }
    else
    {
      double time_since_last_tick = std::chrono::duration< double >(
        pub_time - m_last_measure_time
        ).count();
      // insert time measurement appropriately into window appropriately.
      if( m_time_vec.size() < m_window_size )
      {
        // add measurement
        m_time_vec.push_back( time_since_last_tick );
      }
      else
      {
        // m_frame_time_vec is full, so now we start rotating new-measurement
        // insertion
        m_time_vec[ m_time_vec_i ] = time_since_last_tick;
        m_time_vec_i = ( m_time_vec_i + 1 ) % m_time_vec.size();
      }
    }
    m_last_measure_time = pub_time;
  }

  /// Get the average time delta between ticks within our window of
  /// measurements.
  ///
  /// If there have been no measurements taken yet (by calling `tick()`) then
  /// a -1 value is returned.
  ///
  /// \return Average time in seconds between tick measurements.
  double
  get_delta_avg()
  {
    double avg_time = -1;
    if( not m_time_vec.empty() )
    {
      double window_total = std::accumulate( m_time_vec.begin(),
                                             m_time_vec.end(), 0.0 );
      avg_time = window_total / static_cast< double >( m_time_vec.size() );
    }
    return avg_time;
  }

  /// Get the average tick rate from the window of measurements.
  ///
  /// If there have been no measurements taken yet (by calling `tick()`) then
  /// a -1 value is returned.
  ///
  /// \return Average rate in Hz between tick measurements.
  double
  get_rate_avg()
  {
    double avg_rate = -1;
    if( not m_time_vec.empty() )
    {
      auto avg_time = get_delta_avg();
      avg_rate = 1.0 / avg_time;
    }
    return avg_rate;
  }

private:
  // Type of clock to use for tick rate tracking.
  using clock_t = std::chrono::high_resolution_clock;

  // The measurement window size for rate smoothing.
  size_t m_window_size = 10;
  // If we have not received a tick yet.
  bool m_first_tick = true;
  // The last a tick occurred.
  std::chrono::time_point< clock_t > m_last_measure_time;
  // Circular buffer of time measurements between ticks.
  std::vector< double > m_time_vec;
  // When the m_time_vec circular buffer is full, this is the next index to
  // write to.
  size_t m_time_vec_i = 0;
};

// ----------------------------------------------------------------------------
/// Overlay input object detections onto the image that they were predicted
/// over.
///
/// Overlaying of detections occurs upon receiving a detection set.
///
/// Overlaying may fail if we don't have the image the parent image the
/// detections were predicted upon. If this occurrs, the detection set is
/// simply dropped.
///
/// Assumes that images and headset have the same capture times. If this
/// becomes NOT the case in the future, then a setup using float-comparison,
/// most-recent-fallback will need to be added.
class Simple2dDetectionOverlay
  : public rclcpp::Node
{
public:
  Simple2dDetectionOverlay( rclcpp::NodeOptions const& options );
  ~Simple2dDetectionOverlay() override = default;

  /// Receive an image, overlaying current detections, and emitting an overlaid
  /// version to our output topic.
  void collect_images( sensor_msgs::msg::Image::SharedPtr const image_msg );

  /// Receive and record emitted detections.
  void collect_detections( ObjectDetection2dSet::SharedPtr const det_set );

private:
  // Maximum number of images to retain as "history" for incoming detection set
  // messages to potentialy match against.
  size_t m_max_image_history;

  rclcpp::Subscription< sensor_msgs::msg::Image >::SharedPtr m_sub_input_image;
  rclcpp::Subscription< ObjectDetection2dSet >::SharedPtr m_sub_input_det_2d;
  rclcpp::Publisher< sensor_msgs::msg::Image >::SharedPtr m_pub_overlay_image;

  // Simple counter for image messages received.
  size_t m_image_count = 0;
  // Simple counter for 2D detection messages received.
  size_t m_detset_count = 0;

  // Measure and report receive/publish FPS - RGB Images
  RateTracker m_img_rate_tracker;
  // Measure and report receive/publish FPS - 2D Object Detections
  RateTracker m_det_rate_tracker;

  // Type tracking mapping nanoseconds integer key to the image message.
  using frame_map_t = std::map< size_t, sensor_msgs::msg::Image::SharedPtr >;

  frame_map_t m_frame_map;
};

// ----------------------------------------------------------------------------
Simple2dDetectionOverlay
::Simple2dDetectionOverlay( rclcpp::NodeOptions const& options )
  : Node( "Simple2dDetectionOverlay", options )
{
  auto log = this->get_logger();

  // This two-stage declare->get allows the lack of passing a parameter to
  // throw an error with the parameter name in the error so the user has a
  // clue what is going wrong.
  declare_parameter( PARAM_TOPIC_INPUT_IMAGES );
  declare_parameter( PARAM_TOPIC_INPUT_DET_2D );
  declare_parameter( PARAM_TOPIC_OUTPUT_IMAGE );
  declare_parameter( PARAM_MAX_IMAGE_HISTORY, 30 );

  auto topic_input_images =
    this->get_parameter( PARAM_TOPIC_INPUT_IMAGES ).as_string();
  auto topic_input_detections_2d =
    this->get_parameter( PARAM_TOPIC_INPUT_DET_2D ).as_string();
  auto topic_output_image =
    this->get_parameter( PARAM_TOPIC_OUTPUT_IMAGE ).as_string();

  m_max_image_history =
    get_parameter( PARAM_MAX_IMAGE_HISTORY ).get_value< size_t >();
  if( m_max_image_history <= 0 )
  {
    std::stringstream ss;
    ss  << "Invalid max image history size, must be greater than 0, given "
        << m_max_image_history;
    throw std::invalid_argument( ss.str() );
  }

  // Rate trackers
  m_img_rate_tracker = RateTracker(
    10 // this->get_parameter( PARAM_IMAGE_RATE_WINDOW ).get_value< size_t >()
    );
  m_det_rate_tracker = RateTracker(
    10 // this->get_parameter( PARAM_DET2D_RATE_WINDOW ).get_value< size_t >()
    );

  RCLCPP_INFO( log, "Creating subscribers and publishers" );
  // Alternative "best effort" QoS: rclcpp::SensorDataQoS().keep_last( 1 )
  m_sub_input_image = this->create_subscription< sensor_msgs::msg::Image >(
    topic_input_images, 1,
    std::bind( &Simple2dDetectionOverlay::collect_images, this, _1 )
    );
  m_sub_input_det_2d = this->create_subscription< ObjectDetection2dSet >(
    topic_input_detections_2d, 1,
    std::bind( &Simple2dDetectionOverlay::collect_detections, this, _1 )
    );
  m_pub_overlay_image = this->create_publisher< sensor_msgs::msg::Image >(
    topic_output_image, 1
    );
}

// ----------------------------------------------------------------------------
void
Simple2dDetectionOverlay
::collect_images( sensor_msgs::msg::Image::SharedPtr const image_msg )
{
  auto log = this->get_logger();

  size_t image_nanosec_key = time_key_from_header( image_msg->header.stamp );
  RCLCPP_DEBUG( log, "Received image with time key: %zu", image_nanosec_key );

  // If we have some frames, only retain this image if it newer than the oldest
  // one we have.
  if( m_frame_map.size() > 0 )
  {
    size_t oldest_in_history =
      time_key_from_header( m_frame_map.begin()->second->header.stamp );
    if( image_nanosec_key <= oldest_in_history )
    {
      RCLCPP_WARN(
        log,
        "Received frame has older time-key (%zu) than the current oldest "
        "image in our history (%zu). ",
        image_nanosec_key, oldest_in_history );
      return;
    }
  }

  // If this image has the same key as something in the map something weird
  // happened? Unsure of the pathology of this, so just warning for now.
  auto find_it = m_frame_map.find( image_nanosec_key );
  if( find_it != m_frame_map.end() )
  {
    RCLCPP_WARN(
      log,
      "Incoming frame has key (%zu) already in our history map. What? "
      "Skipping for now...",
      image_nanosec_key
      );
    return;
  }

  // TODO: Drop the image if it is not newer than the latest entry in history?
  //       Would make the above check irrelevant.

  // If we've got too much, remove the lowest key-valued entry (oldest image)
  // Since std::map is ordered, map.begin() when size>0 references the first
  // element, which is what we want to remove because it will have the
  // least-valued key.
  if( m_frame_map.size() == m_max_image_history )
  {
    m_frame_map.erase( m_frame_map.begin() );
  }

  // Finally, insert this frame into our history.
  m_frame_map.insert( { image_nanosec_key, image_msg } );
  RCLCPP_DEBUG( log, "Frame map size: %d", m_frame_map.size() );

  // Because we like to know how fast this is going.
  m_img_rate_tracker.tick();
  RCLCPP_INFO( log,
               "Collected Image #%d (hz: %f)",
               m_image_count, m_img_rate_tracker.get_rate_avg() );
  ++m_image_count;
}

// ----------------------------------------------------------------------------
void
Simple2dDetectionOverlay
::collect_detections( ObjectDetection2dSet::SharedPtr const det_set )
{
  auto log = this->get_logger();

  // lookup image for det_set->source_stamp
  // check that detection header frame_id matches
  size_t source_nanosec_key =
    time_key_from_header( det_set->source_stamp );
  RCLCPP_INFO( log, "Detection source key: %zu", source_nanosec_key );

  // Check that we have the image in our history that matches our detection set
  // received.
  auto find_it = m_frame_map.find( source_nanosec_key );
  if( find_it == m_frame_map.end() )
  {
    auto history_min_key = m_frame_map.begin()->first;
    RCLCPP_WARN(
      log,
      "Failed to find an image in our history that matches received detection "
      "set. D-Set source (%zu) < (%zu) history min.",
      source_nanosec_key, history_min_key
      );
    return;
  }

  // Found something. Double-checking frame source is set to the same thing?
  if( det_set->header.frame_id != find_it->second->header.frame_id )
  {
    RCLCPP_WARN(
      log,
      "Received detection-set frame-id does not match the aligned image in "
      "our history? d-set (%s) != (%s) image",
      det_set->header.frame_id, find_it->second->header.frame_id
      );
    return;
  }

  // Make a copy of the message image to plot on.
  cv_bridge::CvImagePtr img_ptr =
    cv_bridge::toCvCopy( find_it->second, "rgb8" );

  size_t num_detections = det_set->num_detections;
  size_t num_labels = det_set->label_vec.size();
  auto det_label_conf_mat = cv::Mat{
    (int) num_detections, (int) num_labels, CV_64F,
    det_set->label_confidences.data() };

  // Draw on our copied-from-message image detection rectangles.
  static auto const COLOR_BOX = cv::Scalar{ 255, 0, 255 }; // magenta
  static auto const COLOR_TEXT = cv::Scalar{ 255, 255, 255 }; // white
  int line_thickness = thickness_for_drawing( img_ptr->image );
  RCLCPP_DEBUG( log, "Using line thickness: %d", line_thickness );

  cv::Point max_point;  // Only x will be populated due to single row scan.
  std::string max_label;
  for( size_t i = 0; i < num_detections; ++i )
  {
    // Determine the label to apply to the image based on max confidence item.
    cv::minMaxLoc( det_label_conf_mat.row( i ), NULL, NULL, NULL, &max_point );
    max_label = det_set->label_vec[ max_point.x ];

    // Draw the stuff
    cv::Point pt_ul = { (int) round( det_set->left[ i ] ),
                        (int) round( det_set->top[ i ] ) },
              pt_br = { (int) round( det_set->right[ i ] ),
                        (int) round( det_set->bottom[ i ] ) };
    cv::rectangle( img_ptr->image, pt_ul, pt_br,
                   COLOR_BOX, line_thickness, cv::LINE_8 );
    cv::putText( img_ptr->image, max_label, pt_ul,
                 cv::FONT_HERSHEY_COMPLEX, line_thickness, COLOR_TEXT,
                 line_thickness );
  }

  // TODO Use image-transport
  auto out_img_msg = img_ptr->toImageMsg();
  m_pub_overlay_image->publish( *out_img_msg );

  // Because we like to know how fast this is going.
  m_det_rate_tracker.tick();
  RCLCPP_INFO( log, "Plotted detection set #%d (hz: %f)",
               m_detset_count, m_det_rate_tracker.get_rate_avg() );
  ++m_detset_count;
}

} // namespace angel_debug

RCLCPP_COMPONENTS_REGISTER_NODE( angel_debug::Simple2dDetectionOverlay )
