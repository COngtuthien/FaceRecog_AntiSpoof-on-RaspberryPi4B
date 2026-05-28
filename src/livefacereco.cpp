// //
// // Optimized Realtime Live Face Recognition GUI for Raspberry Pi 4 + CSI OV5647
// // - Capture CSI camera using rpicam-vid YUV420 pipe
// // - Load registered users from users/ folder
// // - Process inference on resized frame for better FPS
// // - Run heavy models every N frames and cache result
// // - Show GUI with FPS, identity, recognition score, liveness score
// // - Show TRUE FACE / FAKE FACE
// // - Press q or ESC to quit
// // - Press s to save current aligned face into users/
// //

// #include <math.h>
// #include <time.h>

// #include <algorithm>
// #include <chrono>
// #include <cstdio>
// #include <cstdlib>
// #include <cstring>
// #include <iomanip>
// #include <iostream>
// #include <map>
// #include <sstream>
// #include <string>
// #include <sys/stat.h>
// #include <sys/types.h>
// #include <vector>

// #include "livefacereco.hpp"
// #include "math.hpp"
// #include "mtcnn_new.h"
// #include "FacePreprocess.h"
// // #include "DatasetHandler/image_dataset_handler.hpp"

// #define PI 3.14159265

// using namespace std;
// using namespace cv;

// // ====================== CONFIG ======================
// //
// // Nhớ sửa project_path trong:
// // 1. src/livefacereco.hpp
// // 2. src/arcface.h
// //
// // Ví dụ:
// // const string project_path="/home/pi4/LiveFaceReco_RaspberryPi";
// //
// // ====================================================

// static const std::string USER_DIR = project_path + "/users";

// // Camera CSI OV5647 capture size.
// // Nếu muốn nhanh hơn nữa, đổi CAMERA_WIDTH/HEIGHT thành 320x240.
// // Nếu muốn ảnh đẹp hơn, giữ 640x480 và process ở 320x240.
// static const int CAMERA_WIDTH  = 640;
// static const int CAMERA_HEIGHT = 480;
// static const int CAMERA_FPS    = 30;

// // Kích thước đưa vào MTCNN/ArcFace/Anti-spoof.
// // Đây là điểm tối ưu chính.
// static const int PROCESS_WIDTH  = 320;
// static const int PROCESS_HEIGHT = 240;

// // Chỉ chạy full model mỗi N frame.
// // 3 là cân bằng tốt cho Pi 4. Muốn nhanh hơn nữa thì để 4 hoặc 5.
// static const int PROCESS_EVERY_N_FRAMES = 3;

// // Tên dùng khi bấm phím s để lưu mặt mới.
// static const std::string SAVE_NAME = "Cong";

// // GUI size
// static const int DISPLAY_WIDTH  = 960;
// static const int DISPLAY_HEIGHT = 720;

// // Vẽ tất cả bbox detect được
// static const bool DRAW_ALL_FACES = true;

// // In log terminal mỗi N frame
// static const int LOG_EVERY_N_FRAMES = 15;

// // rpicam stderr log
// static const std::string RPICAM_LOG = "/tmp/rpicam_livefacereco.log";

// // ====================================================

// double sum_score, sum_fps, sum_confidence;

// struct MatchResult
// {
//     std::string name;
//     double score;
//     bool known;
// };

// struct CachedResult
// {
//     bool has_face = false;
//     bool has_aligned_face = false;
//     MatchResult match = {"Unknown", -1.0, false};
//     double live_score = 0.0;
//     bool is_real = false;
//     Bbox process_box;
//     Bbox display_box;
//     int face_count = 0;
//     cv::Mat aligned_face;
//     double last_infer_ms = 0.0;
// };

// // ====================== RPICAM CAPTURE ======================

// class RpiCamRawCapture
// {
// public:
//     RpiCamRawCapture(int width, int height, int fps)
//         : width_(width),
//           height_(height),
//           fps_(fps),
//           frame_size_(static_cast<size_t>(width) * height * 3 / 2),
//           pipe_(nullptr),
//           opened_(false)
//     {
//     }

//     bool open()
//     {
//         std::ostringstream cmd;

//         cmd << "rpicam-vid "
//             << "-t 0 "
//             << "--width " << width_ << " "
//             << "--height " << height_ << " "
//             << "--framerate " << fps_ << " "
//             << "--codec yuv420 "
//             << "-n "
//             << "-o - "
//             << "2>" << RPICAM_LOG;

//         std::cout << "[INFO] Starting CSI camera with command:" << std::endl;
//         std::cout << "[INFO] " << cmd.str() << std::endl;

//         pipe_ = popen(cmd.str().c_str(), "r");

//         if (!pipe_)
//         {
//             std::cerr << "[ERROR] Cannot start rpicam-vid." << std::endl;
//             opened_ = false;
//             return false;
//         }

//         buffer_.resize(frame_size_);
//         opened_ = true;
//         return true;
//     }

//     bool isOpened() const
//     {
//         return opened_ && pipe_ != nullptr;
//     }

//     cv::Mat getFrame()
//     {
//         if (!isOpened())
//         {
//             return cv::Mat();
//         }

//         size_t total_read = 0;

//         while (total_read < frame_size_)
//         {
//             size_t n = fread(
//                 buffer_.data() + total_read,
//                 1,
//                 frame_size_ - total_read,
//                 pipe_
//             );

//             if (n == 0)
//             {
//                 std::cerr << "[ERROR] rpicam stream ended or read failed." << std::endl;
//                 std::cerr << "[HINT] Check log: cat " << RPICAM_LOG << std::endl;
//                 close();
//                 return cv::Mat();
//             }

//             total_read += n;
//         }

//         cv::Mat yuv(
//             height_ * 3 / 2,
//             width_,
//             CV_8UC1,
//             buffer_.data()
//         );

//         cv::Mat bgr;
//         cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);

//         return bgr.clone();
//     }

//     void close()
//     {
//         if (pipe_)
//         {
//             pclose(pipe_);
//             pipe_ = nullptr;
//         }

//         opened_ = false;
//     }

//     ~RpiCamRawCapture()
//     {
//         close();
//     }

// private:
//     int width_;
//     int height_;
//     int fps_;
//     size_t frame_size_;
//     FILE* pipe_;
//     bool opened_;
//     std::vector<unsigned char> buffer_;
// };

// // ====================== UTILS ======================

// static void ensureDir(const std::string& path)
// {
//     mkdir(path.c_str(), 0755);
// }

// static double clamp01(double v)
// {
//     if (v < 0.0) return 0.0;
//     if (v > 1.0) return 1.0;
//     return v;
// }

// static std::string fmtDouble(double v, int precision = 2)
// {
//     std::ostringstream oss;
//     oss << std::fixed << std::setprecision(precision) << v;
//     return oss.str();
// }

// std::vector<std::string> split(const std::string& s, char seperator)
// {
//     std::vector<std::string> output;
//     std::string::size_type prev_pos = 0, pos = 0;

//     while ((pos = s.find(seperator, pos)) != std::string::npos)
//     {
//         std::string substring(s.substr(prev_pos, pos - prev_pos));
//         output.push_back(substring);
//         prev_pos = ++pos;
//     }

//     output.push_back(s.substr(prev_pos, pos - prev_pos));
//     return output;
// }

// static std::string basenameFromPath(const std::string& path)
// {
//     std::vector<std::string> parts = split(path, '/');
//     if (parts.empty()) return path;
//     return parts.back();
// }

// static std::string removeExtension(const std::string& filename)
// {
//     std::size_t dot_pos = filename.find_last_of('.');
//     if (dot_pos == std::string::npos) return filename;
//     return filename.substr(0, dot_pos);
// }

// static std::string extractPersonName(const std::string& img_path)
// {
//     std::string filename = basenameFromPath(img_path);
//     std::string name_no_ext = removeExtension(filename);

//     std::size_t underscore_pos = name_no_ext.find_last_of('_');
//     if (underscore_pos == std::string::npos)
//     {
//         return name_no_ext;
//     }

//     return name_no_ext.substr(0, underscore_pos);
// }

// // ====================== FACE HELPERS ======================

// cv::Mat createFaceLandmarkGTMatrix()
// {
//     float v1[5][2] = {
//         {30.2946f, 51.6963f},
//         {65.5318f, 51.5014f},
//         {48.0252f, 71.7366f},
//         {33.5493f, 92.3655f},
//         {62.7299f, 92.2041f}
//     };

//     cv::Mat src(5, 2, CV_32FC1, v1);
//     memcpy(src.data, v1, 2 * 5 * sizeof(float));
//     return src.clone();
// }

// cv::Mat createFaceLandmarkMatrixfromBBox(const Bbox& box)
// {
//     float v2[5][2] = {
//         {box.ppoint[0], box.ppoint[5]},
//         {box.ppoint[1], box.ppoint[6]},
//         {box.ppoint[2], box.ppoint[7]},
//         {box.ppoint[3], box.ppoint[8]},
//         {box.ppoint[4], box.ppoint[9]},
//     };

//     cv::Mat dst(5, 2, CV_32FC1, v2);
//     memcpy(dst.data, v2, 2 * 5 * sizeof(float));
//     return dst.clone();
// }

// Bbox getLargestBboxFromBboxVec(const std::vector<Bbox>& faces_info)
// {
//     if (faces_info.empty())
//     {
//         return Bbox();
//     }

//     int largest_idx = 0;
//     float largest_area = 0.0f;

//     for (int i = 0; i < (int)faces_info.size(); i++)
//     {
//         float w = faces_info[i].x2 - faces_info[i].x1;
//         float h = faces_info[i].y2 - faces_info[i].y1;
//         float area = w * h;

//         if (area > largest_area)
//         {
//             largest_area = area;
//             largest_idx = i;
//         }
//     }

//     return faces_info[largest_idx];
// }

// static Bbox scaleBbox(const Bbox& box, float sx, float sy)
// {
//     Bbox out = box;

//     out.x1 = box.x1 * sx;
//     out.y1 = box.y1 * sy;
//     out.x2 = box.x2 * sx;
//     out.y2 = box.y2 * sy;

//     for (int i = 0; i < 5; i++)
//     {
//         out.ppoint[i] = box.ppoint[i] * sx;
//         out.ppoint[i + 5] = box.ppoint[i + 5] * sy;
//     }

//     return out;
// }

// LiveFaceBox Bbox2LiveFaceBox(const Bbox& box)
// {
//     LiveFaceBox live_box = {box.x1, box.y1, box.x2, box.y2};
//     return live_box;
// }

// cv::Mat alignFaceImage(
//     const cv::Mat& frame,
//     const Bbox& bbox,
//     const cv::Mat& gt_landmark_matrix
// )
// {
//     cv::Mat face_landmark = createFaceLandmarkMatrixfromBBox(bbox);

//     cv::Mat transf = FacePreprocess::similarTransform(
//         face_landmark,
//         gt_landmark_matrix
//     );

//     cv::Mat aligned = frame.clone();

//     cv::warpPerspective(
//         frame,
//         aligned,
//         transf,
//         cv::Size(96, 112),
//         INTER_LINEAR
//     );

//     cv::resize(
//         aligned,
//         aligned,
//         cv::Size(112, 112),
//         0,
//         0,
//         INTER_LINEAR
//     );

//     return aligned.clone();
// }

// // ====================== DRAW HELPERS ======================

// static void drawTextWithBg(
//     cv::Mat& img,
//     const std::string& text,
//     cv::Point org,
//     double scale,
//     cv::Scalar text_color,
//     cv::Scalar bg_color,
//     int thickness = 2
// )
// {
//     int baseline = 0;

//     cv::Size text_size = cv::getTextSize(
//         text,
//         cv::FONT_HERSHEY_SIMPLEX,
//         scale,
//         thickness,
//         &baseline
//     );

//     cv::Rect bg_rect(
//         org.x - 4,
//         org.y - text_size.height - 6,
//         text_size.width + 8,
//         text_size.height + baseline + 10
//     );

//     bg_rect &= cv::Rect(0, 0, img.cols, img.rows);

//     cv::rectangle(img, bg_rect, bg_color, cv::FILLED);

//     cv::putText(
//         img,
//         text,
//         org,
//         cv::FONT_HERSHEY_SIMPLEX,
//         scale,
//         text_color,
//         thickness
//     );
// }

// static void drawAllDetectedFaces(
//     cv::Mat& frame,
//     const std::vector<Bbox>& faces
// )
// {
//     for (const auto& box : faces)
//     {
//         cv::rectangle(
//             frame,
//             cv::Point((int)box.x1, (int)box.y1),
//             cv::Point((int)box.x2, (int)box.y2),
//             cv::Scalar(160, 160, 160),
//             1
//         );
//     }
// }

// static void drawMainFaceResult(
//     cv::Mat& frame,
//     const CachedResult& cached,
//     double fps,
//     double loop_ms
// )
// {
//     const Bbox& box = cached.display_box;
//     const MatchResult& match = cached.match;

//     bool unlock_ok = cached.is_real && match.known;

//     cv::Scalar box_color;
//     cv::Scalar status_bg;

//     if (unlock_ok)
//     {
//         box_color = cv::Scalar(0, 255, 0);
//         status_bg = cv::Scalar(0, 120, 0);
//     }
//     else if (!cached.is_real)
//     {
//         box_color = cv::Scalar(0, 0, 255);
//         status_bg = cv::Scalar(0, 0, 160);
//     }
//     else
//     {
//         box_color = cv::Scalar(0, 255, 255);
//         status_bg = cv::Scalar(0, 120, 120);
//     }

//     cv::rectangle(
//         frame,
//         cv::Point((int)box.x1, (int)box.y1),
//         cv::Point((int)box.x2, (int)box.y2),
//         box_color,
//         3
//     );

//     std::string status_text;

//     if (!cached.is_real)
//     {
//         status_text = "FAKE FACE";
//     }
//     else if (match.known)
//     {
//         status_text = "TRUE FACE - REGISTERED";
//     }
//     else
//     {
//         status_text = "TRUE FACE - UNKNOWN";
//     }

//     std::string name_text =
//         "Name: " + match.name;

//     std::string rec_text =
//         "Rec score: " + fmtDouble(match.score, 3) +
//         " / Th: " + fmtDouble(face_thre, 2);

//     std::string live_text =
//         "Live score: " + fmtDouble(cached.live_score, 3) +
//         " / Th: " + fmtDouble(true_thre, 2);

//     std::string perf_text =
//         "FPS: " + fmtDouble(fps, 1) +
//         " | Loop: " + fmtDouble(loop_ms, 1) + " ms" +
//         " | Infer: " + fmtDouble(cached.last_infer_ms, 1) + " ms" +
//         " | Step: " + std::to_string(PROCESS_EVERY_N_FRAMES);

//     std::string face_text =
//         "Faces: " + std::to_string(cached.face_count);

//     int x = std::max(10, (int)box.x1);
//     int y = std::max(30, (int)box.y1 - 15);

//     drawTextWithBg(
//         frame,
//         status_text,
//         cv::Point(x, y),
//         0.75,
//         cv::Scalar(255, 255, 255),
//         status_bg,
//         2
//     );

//     drawTextWithBg(
//         frame,
//         name_text,
//         cv::Point(x, y + 35),
//         0.65,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(40, 40, 40),
//         2
//     );

//     drawTextWithBg(
//         frame,
//         rec_text,
//         cv::Point(x, y + 65),
//         0.55,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(40, 40, 40),
//         1
//     );

//     drawTextWithBg(
//         frame,
//         live_text,
//         cv::Point(x, y + 92),
//         0.55,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(40, 40, 40),
//         1
//     );

//     drawTextWithBg(
//         frame,
//         perf_text + " | " + face_text,
//         cv::Point(15, 35),
//         0.62,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 0),
//         2
//     );

//     drawTextWithBg(
//         frame,
//         "Press q/ESC: quit | Press s: save current face",
//         cv::Point(15, frame.rows - 20),
//         0.55,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 0),
//         1
//     );
// }

// static void drawNoFaceUI(
//     cv::Mat& frame,
//     double fps,
//     double infer_ms,
//     double loop_ms
// )
// {
//     drawTextWithBg(
//         frame,
//         "No face detected",
//         cv::Point(15, 35),
//         0.8,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 160),
//         2
//     );

//     drawTextWithBg(
//         frame,
//         "FPS: " + fmtDouble(fps, 1) +
//         " | Loop: " + fmtDouble(loop_ms, 1) + " ms" +
//         " | Infer: " + fmtDouble(infer_ms, 1) + " ms" +
//         " | Step: " + std::to_string(PROCESS_EVERY_N_FRAMES),
//         cv::Point(15, 70),
//         0.6,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 0),
//         2
//     );

//     drawTextWithBg(
//         frame,
//         "Press q/ESC: quit",
//         cv::Point(15, frame.rows - 20),
//         0.55,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 0),
//         1
//     );
// }

// // ====================== MODEL / USER DB ======================

// void loadLiveModel(Live& live)
// {
//     ModelConfig config1 = {
//         2.7f,
//         0.0f,
//         0.0f,
//         80,
//         80,
//         "model_1",
//         false
//     };

//     ModelConfig config2 = {
//         4.0f,
//         0.0f,
//         0.0f,
//         80,
//         80,
//         "model_2",
//         false
//     };

//     vector<ModelConfig> configs;
//     configs.emplace_back(config1);
//     configs.emplace_back(config2);

//     live.LoadModel(configs);
// }

// static cv::Mat prepareUserImageForFeature(
//     const cv::Mat& input_img,
//     const cv::Mat& gt_landmark_matrix
// )
// {
//     if (input_img.empty())
//     {
//         return cv::Mat();
//     }

//     if (input_img.cols == 112 && input_img.rows == 112)
//     {
//         return input_img.clone();
//     }

//     std::vector<Bbox> faces = detect_mtcnn(input_img);

//     if (!faces.empty())
//     {
//         Bbox largest = getLargestBboxFromBboxVec(faces);
//         return alignFaceImage(input_img, largest, gt_landmark_matrix);
//     }

//     cv::Mat resized;
//     cv::resize(
//         input_img,
//         resized,
//         cv::Size(112, 112),
//         0,
//         0,
//         INTER_LINEAR
//     );

//     return resized;
// }

// static void loadRegisteredUsers(
//     Arcface& facereco,
//     std::map<std::string, std::vector<cv::Mat>>& user_descriptors,
//     const cv::Mat& gt_landmark_matrix
// )
// {
//     ensureDir(USER_DIR);

//     std::vector<cv::String> image_names;
//     std::vector<cv::String> jpg_names;
//     std::vector<cv::String> jpeg_names;
//     std::vector<cv::String> png_names;

//     cv::glob(USER_DIR + "/*.jpg", jpg_names, false);
//     cv::glob(USER_DIR + "/*.jpeg", jpeg_names, false);
//     cv::glob(USER_DIR + "/*.png", png_names, false);

//     image_names.insert(image_names.end(), jpg_names.begin(), jpg_names.end());
//     image_names.insert(image_names.end(), jpeg_names.begin(), jpeg_names.end());
//     image_names.insert(image_names.end(), png_names.begin(), png_names.end());

//     if (image_names.empty())
//     {
//         std::cerr << "[ERROR] No image files found in: "
//                   << USER_DIR << std::endl;
//         std::cerr << "[HINT] Put registered face images into users/ folder."
//                   << std::endl;
//         std::cerr << "[HINT] Example: users/Cong_001.jpg, users/Duc_001.jpg"
//                   << std::endl;
//         exit(0);
//     }

//     std::cout << "[INFO] Loading registered users from: "
//               << USER_DIR << std::endl;
//     std::cout << "[INFO] Total user images: "
//               << image_names.size() << std::endl;

//     int loaded = 0;
//     int skipped = 0;

//     for (const auto& img_name_cv : image_names)
//     {
//         std::string img_name = std::string(img_name_cv);
//         std::string person_name = extractPersonName(img_name);

//         cv::Mat img = cv::imread(img_name);

//         if (img.empty())
//         {
//             std::cerr << "[WARN] Cannot read image: "
//                       << img_name << std::endl;
//             skipped++;
//             continue;
//         }

//         cv::Mat face_img = prepareUserImageForFeature(
//             img,
//             gt_landmark_matrix
//         );

//         if (face_img.empty())
//         {
//             std::cerr << "[WARN] Cannot prepare face image: "
//                       << img_name << std::endl;
//             skipped++;
//             continue;
//         }

//         cv::Mat descriptor = facereco.getFeature(face_img);
//         descriptor = Statistics::zScore(descriptor);

//         user_descriptors[person_name].push_back(descriptor);
//         loaded++;

//         std::cout << "[LOAD] "
//                   << person_name
//                   << " <= "
//                   << basenameFromPath(img_name)
//                   << std::endl;
//     }

//     std::cout << "[INFO] Loaded descriptors: "
//               << loaded << std::endl;
//     std::cout << "[INFO] Skipped images: "
//               << skipped << std::endl;
//     std::cout << "[INFO] Registered people: "
//               << user_descriptors.size() << std::endl;

//     if (user_descriptors.empty())
//     {
//         std::cerr << "[ERROR] No valid user descriptor loaded."
//                   << std::endl;
//         exit(0);
//     }
// }

// static MatchResult findBestMatch(
//     const std::map<std::string, std::vector<cv::Mat>>& user_descriptors,
//     const cv::Mat& face_descriptor
// )
// {
//     MatchResult result;
//     result.name = "Unknown";
//     result.score = -1.0;
//     result.known = false;

//     for (const auto& user_pair : user_descriptors)
//     {
//         const std::string& name = user_pair.first;
//         const std::vector<cv::Mat>& descriptors = user_pair.second;

//         for (const auto& ref_desc : descriptors)
//         {
//             double score = Statistics::cosineDistance(
//                 ref_desc,
//                 face_descriptor
//             );

//             if (score > result.score)
//             {
//                 result.score = score;
//                 result.name = name;
//             }
//         }
//     }

//     if (result.score > face_thre)
//     {
//         result.known = true;
//     }
//     else
//     {
//         result.name = "Unknown";
//         result.known = false;
//     }

//     return result;
// }

// static std::string makeSaveFileName(const std::string& name)
// {
//     auto now = std::chrono::system_clock::now();

//     long long ms =
//         std::chrono::duration_cast<std::chrono::milliseconds>(
//             now.time_since_epoch()
//         ).count();

//     return USER_DIR + "/" + name + "_" + std::to_string(ms) + ".jpg";
// }

// // ====================== MAIN PIPELINE ======================

// int MTCNNDetection()
// {
//     std::cout << "OpenCV Version: "
//               << CV_MAJOR_VERSION << "."
//               << CV_MINOR_VERSION << "."
//               << CV_SUBMINOR_VERSION << std::endl;

//     std::cout << "[INFO] project_path = "
//               << project_path << std::endl;
//     std::cout << "[INFO] USER_DIR     = "
//               << USER_DIR << std::endl;
//     std::cout << "[INFO] CSI CAMERA   = "
//               << CAMERA_WIDTH << "x" << CAMERA_HEIGHT
//               << "@" << CAMERA_FPS << std::endl;
//     std::cout << "[INFO] PROCESS SIZE = "
//               << PROCESS_WIDTH << "x" << PROCESS_HEIGHT << std::endl;
//     std::cout << "[INFO] PROCESS STEP = every "
//               << PROCESS_EVERY_N_FRAMES << " frames" << std::endl;
//     std::cout << "[INFO] face_thre    = "
//               << face_thre << std::endl;
//     std::cout << "[INFO] true_thre    = "
//               << true_thre << std::endl;

//     ensureDir(USER_DIR);

//     cv::Mat face_landmark_gt_matrix = createFaceLandmarkGTMatrix();

//     Arcface facereco;

//     std::map<std::string, std::vector<cv::Mat>> user_descriptors;

//     loadRegisteredUsers(
//         facereco,
//         user_descriptors,
//         face_landmark_gt_matrix
//     );

//     Live live;
//     loadLiveModel(live);

//     RpiCamRawCapture cap(CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS);

//     if (!cap.open())
//     {
//         std::cerr << "[ERROR] Cannot open CSI camera using rpicam-vid."
//                   << std::endl;
//         std::cerr << "[HINT] Test camera first:" << std::endl;
//         std::cerr << "       rpicam-hello --list-cameras" << std::endl;
//         std::cerr << "       rpicam-hello" << std::endl;
//         std::cerr << "[HINT] Check log:" << std::endl;
//         std::cerr << "       cat " << RPICAM_LOG << std::endl;
//         return -1;
//     }

//     cv::namedWindow("LiveFaceReco - Smart Lock", cv::WINDOW_NORMAL);
//     cv::resizeWindow(
//         "LiveFaceReco - Smart Lock",
//         DISPLAY_WIDTH,
//         DISPLAY_HEIGHT
//     );

//     cv::Mat frame;
//     cv::Mat process_frame;
//     cv::Mat display_frame;

//     CachedResult cached;

//     int frame_count = 0;
//     double smooth_fps = 0.0;
//     double last_infer_ms = 0.0;
//     double last_loop_ms = 0.0;

//     std::cout << "[INFO] CSI camera started." << std::endl;
//     std::cout << "[INFO] Press q or ESC to quit." << std::endl;
//     std::cout << "[INFO] Press s to save current aligned face into users/."
//               << std::endl;

//     while (cap.isOpened())
//     {
//         auto loop_start = std::chrono::steady_clock::now();

//         frame = cap.getFrame();

//         if (frame.empty())
//         {
//             continue;
//         }

//         frame_count++;

//         display_frame = frame.clone();

//         bool should_process =
//             (frame_count % PROCESS_EVERY_N_FRAMES == 0) ||
//             (frame_count == 1);

//         if (should_process)
//         {
//             auto infer_start = std::chrono::high_resolution_clock::now();

//             cv::resize(
//                 frame,
//                 process_frame,
//                 cv::Size(PROCESS_WIDTH, PROCESS_HEIGHT),
//                 0,
//                 0,
//                 INTER_LINEAR
//             );

//             float sx = static_cast<float>(frame.cols) / PROCESS_WIDTH;
//             float sy = static_cast<float>(frame.rows) / PROCESS_HEIGHT;

//             std::vector<Bbox> faces_info = detect_mtcnn(process_frame);

//             if (!faces_info.empty())
//             {
//                 Bbox largest_process_box =
//                     getLargestBboxFromBboxVec(faces_info);

//                 Bbox largest_display_box =
//                     scaleBbox(largest_process_box, sx, sy);

//                 LiveFaceBox live_face_box =
//                     Bbox2LiveFaceBox(largest_process_box);

//                 cv::Mat aligned_img = alignFaceImage(
//                     process_frame,
//                     largest_process_box,
//                     face_landmark_gt_matrix
//                 );

//                 cv::Mat face_descriptor = facereco.getFeature(aligned_img);
//                 face_descriptor = Statistics::zScore(face_descriptor);

//                 MatchResult match = findBestMatch(
//                     user_descriptors,
//                     face_descriptor
//                 );

//                 double live_score =
//                     live.Detect(process_frame, live_face_box);

//                 live_score = clamp01(live_score);
//                 bool is_real = live_score > true_thre;

//                 cached.has_face = true;
//                 cached.has_aligned_face = true;
//                 cached.match = match;
//                 cached.live_score = live_score;
//                 cached.is_real = is_real;
//                 cached.process_box = largest_process_box;
//                 cached.display_box = largest_display_box;
//                 cached.face_count = (int)faces_info.size();
//                 cached.aligned_face = aligned_img.clone();

//                 if (DRAW_ALL_FACES)
//                 {
//                     // Chỉ để debug nhẹ: update display bbox lớn nhất thôi.
//                     // Nếu muốn vẽ tất cả bbox, cần scale từng bbox.
//                 }
//             }
//             else
//             {
//                 cached.has_face = false;
//                 cached.has_aligned_face = false;
//                 cached.match = {"Unknown", -1.0, false};
//                 cached.live_score = 0.0;
//                 cached.is_real = false;
//                 cached.face_count = 0;
//                 cached.aligned_face.release();
//             }

//             auto infer_end = std::chrono::high_resolution_clock::now();

//             last_infer_ms =
//                 std::chrono::duration<double, std::milli>(
//                     infer_end - infer_start
//                 ).count();

//             cached.last_infer_ms = last_infer_ms;
//         }

//         if (cached.has_face)
//         {
//             drawMainFaceResult(
//                 display_frame,
//                 cached,
//                 smooth_fps,
//                 last_loop_ms
//             );
//         }
//         else
//         {
//             drawNoFaceUI(
//                 display_frame,
//                 smooth_fps,
//                 last_infer_ms,
//                 last_loop_ms
//             );
//         }

//         cv::imshow("LiveFaceReco - Smart Lock", display_frame);

//         int key = cv::waitKey(1);

//         // Tính FPS ở cuối vòng lặp để số FPS bao gồm toàn bộ thời gian:
//         // camera read + resize + detect + recognition + anti-spoof + draw + imshow + waitKey.
//         // Cách cũ tính ngay sau cap.getFrame(), nên FPS có thể bị ảo cao dù hình vẫn giật.
//         auto loop_end = std::chrono::steady_clock::now();

//         double loop_dt = std::chrono::duration<double>(
//             loop_end - loop_start
//         ).count();

//         if (loop_dt > 0.000001)
//         {
//             double instant_fps = 1.0 / loop_dt;
//             last_loop_ms = loop_dt * 1000.0;

//             if (smooth_fps <= 0.0)
//             {
//                 smooth_fps = instant_fps;
//             }
//             else
//             {
//                 const double alpha = 0.10;
//                 smooth_fps =
//                     (1.0 - alpha) * smooth_fps +
//                     alpha * instant_fps;
//             }
//         }

//         if (key == 27 || key == 'q' || key == 'Q')
//         {
//             break;
//         }

//         if (key == 's' || key == 'S')
//         {
//             if (cached.has_aligned_face && !cached.aligned_face.empty())
//             {
//                 std::string save_path = makeSaveFileName(SAVE_NAME);

//                 bool ok = cv::imwrite(save_path, cached.aligned_face);

//                 if (ok)
//                 {
//                     cv::Mat desc = facereco.getFeature(cached.aligned_face);
//                     desc = Statistics::zScore(desc);
//                     user_descriptors[SAVE_NAME].push_back(desc);

//                     std::cout << "[SAVE] Saved new face: "
//                               << save_path << std::endl;
//                     std::cout << "[SAVE] Added descriptor to current database."
//                               << std::endl;
//                 }
//                 else
//                 {
//                     std::cerr << "[ERROR] Cannot save image: "
//                               << save_path << std::endl;
//                 }
//             }
//             else
//             {
//                 std::cout << "[WARN] No current face to save."
//                           << std::endl;
//             }
//         }

//         if (frame_count % LOG_EVERY_N_FRAMES == 0)
//         {
//             std::cout << "[FRAME " << frame_count << "] ";

//             if (cached.has_face)
//             {
//                 std::cout << "name=" << cached.match.name
//                           << " | known="
//                           << (cached.match.known ? "yes" : "no")
//                           << " | rec_score="
//                           << fmtDouble(cached.match.score, 3)
//                           << " | live_score="
//                           << fmtDouble(cached.live_score, 3)
//                           << " | status="
//                           << (cached.is_real ? "TRUE" : "FAKE")
//                           << " | fps="
//                           << fmtDouble(smooth_fps, 1)
//                           << " | loop_ms="
//                           << fmtDouble(last_loop_ms, 1)
//                           << " | infer_ms="
//                           << fmtDouble(cached.last_infer_ms, 1)
//                           << std::endl;
//             }
//             else
//             {
//                 std::cout << "no face"
//                           << " | fps="
//                           << fmtDouble(smooth_fps, 1)
//                           << " | loop_ms="
//                           << fmtDouble(last_loop_ms, 1)
//                           << " | infer_ms="
//                           << fmtDouble(last_infer_ms, 1)
//                           << std::endl;
//             }
//         }
//     }

//     cap.close();
//     cv::destroyAllWindows();

//     std::cout << "[INFO] CSI camera stopped." << std::endl;

//     return 0;
// }











// //
// // Optimized Realtime Live Face Recognition GUI for Raspberry Pi 4 + CSI OV5647
// // - Capture CSI camera using rpicam-vid YUV420 pipe
// // - Use a dedicated camera thread and keep ONLY the latest frame
// // - Avoid processing stale buffered frames from rpicam pipe
// // - Load registered users from users/ folder
// // - Process inference on resized frame for better FPS
// // - Run heavy models every N frames and cache result
// // - GUI only shows FPS, TRUE/FAKE/NO FACE, and recognized name
// // - Terminal log shows FPS, loop time, infer time, recognition score, live score, status, name
// // - Press q or ESC to quit
// // - Press s to save current aligned face into users/
// //

// #include <math.h>
// #include <time.h>

// #include <algorithm>
// #include <atomic>
// #include <chrono>
// #include <condition_variable>
// #include <cstdio>
// #include <cstdlib>
// #include <cstring>
// #include <cstdint>
// #include <iomanip>
// #include <iostream>
// #include <map>
// #include <mutex>
// #include <sstream>
// #include <string>
// #include <sys/stat.h>
// #include <sys/types.h>
// #include <thread>
// #include <vector>

// #include "livefacereco.hpp"
// #include "math.hpp"
// #include "mtcnn_new.h"
// #include "FacePreprocess.h"
// // #include "DatasetHandler/image_dataset_handler.hpp"

// #define PI 3.14159265

// using namespace std;
// using namespace cv;

// // ====================== CONFIG ======================
// //
// // Nhớ sửa project_path trong:
// // 1. src/livefacereco.hpp
// // 2. src/arcface.h
// //
// // Ví dụ:
// // const string project_path="/home/pi4/LiveFaceReco_RaspberryPi";
// //
// // ====================================================

// static const std::string USER_DIR = project_path + "/users";

// // Camera CSI OV5647 capture size.
// static const int CAMERA_WIDTH  = 640;
// static const int CAMERA_HEIGHT = 480;
// static const int CAMERA_FPS    = 60;

// // Kích thước đưa vào MTCNN/ArcFace/Anti-spoof.
// static const int PROCESS_WIDTH  = 320;
// static const int PROCESS_HEIGHT = 240;

// // Chỉ chạy full model mỗi N frame.
// static const int PROCESS_EVERY_N_FRAMES = 3;

// // Tên dùng khi bấm phím s để lưu mặt mới.
// static const std::string SAVE_NAME = "Cong";

// // GUI size
// static const int DISPLAY_WIDTH  = 960;
// static const int DISPLAY_HEIGHT = 720;

// // Không vẽ các bbox phụ để tránh nhìn như có nhiều mặt.
// static const bool DRAW_ALL_FACES = false;

// // In log terminal mỗi N frame
// static const int LOG_EVERY_N_FRAMES = 15;

// // rpicam stderr log
// static const std::string RPICAM_LOG = "/tmp/rpicam_livefacereco.log";

// // ====================================================

// double sum_score, sum_fps, sum_confidence;

// struct MatchResult
// {
//     std::string name;
//     double score;
//     bool known;
// };

// struct CachedResult
// {
//     bool has_face = false;
//     bool has_aligned_face = false;
//     MatchResult match = {"Unknown", -1.0, false};
//     double live_score = 0.0;
//     bool is_real = false;
//     Bbox process_box;
//     Bbox display_box;
//     int face_count = 0;
//     cv::Mat aligned_face;
//     double last_infer_ms = 0.0;
// };

// struct PerfInfo
// {
//     double fps = 0.0;
//     double loop_ms = 0.0;
//     double infer_ms = 0.0;
// };

// // ====================== RPICAM LATEST-FRAME CAPTURE ======================
// //
// // Camera thread luôn đọc rpicam pipe và chỉ giữ frame mới nhất.
// // Main loop lấy latest frame để tránh xử lý frame cũ trong buffer.
// //

// class RpiCamLatestCapture
// {
// public:
//     RpiCamLatestCapture(int width, int height, int fps)
//         : width_(width),
//           height_(height),
//           fps_(fps),
//           frame_size_(static_cast<size_t>(width) * height * 3 / 2),
//           pipe_(nullptr),
//           opened_(false),
//           running_(false),
//           latest_frame_id_(0),
//           delivered_frame_id_(0)
//     {
//     }

//     bool open()
//     {
//         std::ostringstream cmd;

//         cmd << "rpicam-vid "
//             << "-t 0 "
//             << "--width " << width_ << " "
//             << "--height " << height_ << " "
//             << "--framerate " << fps_ << " "
//             << "--codec yuv420 "
//             << "-n "
//             << "-o - "
//             << "2>" << RPICAM_LOG;

//         std::cout << "[INFO] Starting CSI camera with command:" << std::endl;
//         std::cout << "[INFO] " << cmd.str() << std::endl;

//         pipe_ = popen(cmd.str().c_str(), "r");

//         if (!pipe_)
//         {
//             std::cerr << "[ERROR] Cannot start rpicam-vid." << std::endl;
//             opened_ = false;
//             running_ = false;
//             return false;
//         }

//         opened_ = true;
//         running_ = true;

//         capture_thread_ = std::thread(&RpiCamLatestCapture::captureLoop, this);

//         return true;
//     }

//     bool isOpened() const
//     {
//         return opened_ && running_;
//     }

//     bool getFrame(cv::Mat& out_frame, uint64_t& out_frame_id)
//     {
//         std::unique_lock<std::mutex> lock(mutex_);

//         cond_.wait(
//             lock,
//             [this]()
//             {
//                 return !running_ || latest_frame_id_ != delivered_frame_id_;
//             }
//         );

//         if (!running_ && latest_frame_.empty())
//         {
//             return false;
//         }

//         if (latest_frame_.empty())
//         {
//             return false;
//         }

//         out_frame = latest_frame_.clone();
//         delivered_frame_id_ = latest_frame_id_;
//         out_frame_id = latest_frame_id_;

//         return true;
//     }

//     void close()
//     {
//         running_ = false;
//         cond_.notify_all();

//         if (capture_thread_.joinable())
//         {
//             capture_thread_.join();
//         }

//         if (pipe_)
//         {
//             pclose(pipe_);
//             pipe_ = nullptr;
//         }

//         opened_ = false;
//     }

//     ~RpiCamLatestCapture()
//     {
//         close();
//     }

// private:
//     void captureLoop()
//     {
//         std::vector<unsigned char> buffer(frame_size_);

//         while (running_)
//         {
//             size_t total_read = 0;

//             while (running_ && total_read < frame_size_)
//             {
//                 size_t n = fread(
//                     buffer.data() + total_read,
//                     1,
//                     frame_size_ - total_read,
//                     pipe_
//                 );

//                 if (n == 0)
//                 {
//                     if (running_)
//                     {
//                         std::cerr << "[ERROR] rpicam stream ended or read failed." << std::endl;
//                         std::cerr << "[HINT] Check log: cat " << RPICAM_LOG << std::endl;
//                     }

//                     running_ = false;
//                     cond_.notify_all();
//                     return;
//                 }

//                 total_read += n;
//             }

//             if (!running_)
//             {
//                 break;
//             }

//             cv::Mat yuv(
//                 height_ * 3 / 2,
//                 width_,
//                 CV_8UC1,
//                 buffer.data()
//             );

//             cv::Mat bgr;
//             cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);

//             {
//                 std::lock_guard<std::mutex> lock(mutex_);
//                 latest_frame_ = bgr.clone();
//                 latest_frame_id_++;
//             }

//             cond_.notify_one();
//         }

//         cond_.notify_all();
//     }

// private:
//     int width_;
//     int height_;
//     int fps_;
//     size_t frame_size_;

//     FILE* pipe_;
//     bool opened_;
//     std::atomic<bool> running_;

//     std::thread capture_thread_;

//     std::mutex mutex_;
//     std::condition_variable cond_;

//     cv::Mat latest_frame_;
//     uint64_t latest_frame_id_;
//     uint64_t delivered_frame_id_;
// };

// // ====================== UTILS ======================

// static void ensureDir(const std::string& path)
// {
//     mkdir(path.c_str(), 0755);
// }

// static double clamp01(double v)
// {
//     if (v < 0.0) return 0.0;
//     if (v > 1.0) return 1.0;
//     return v;
// }

// static std::string fmtDouble(double v, int precision = 2)
// {
//     std::ostringstream oss;
//     oss << std::fixed << std::setprecision(precision) << v;
//     return oss.str();
// }

// std::vector<std::string> split(const std::string& s, char seperator)
// {
//     std::vector<std::string> output;
//     std::string::size_type prev_pos = 0, pos = 0;

//     while ((pos = s.find(seperator, pos)) != std::string::npos)
//     {
//         std::string substring(s.substr(prev_pos, pos - prev_pos));
//         output.push_back(substring);
//         prev_pos = ++pos;
//     }

//     output.push_back(s.substr(prev_pos, pos - prev_pos));
//     return output;
// }

// static std::string basenameFromPath(const std::string& path)
// {
//     std::vector<std::string> parts = split(path, '/');
//     if (parts.empty()) return path;
//     return parts.back();
// }

// static std::string removeExtension(const std::string& filename)
// {
//     std::size_t dot_pos = filename.find_last_of('.');
//     if (dot_pos == std::string::npos) return filename;
//     return filename.substr(0, dot_pos);
// }

// static std::string extractPersonName(const std::string& img_path)
// {
//     std::string filename = basenameFromPath(img_path);
//     std::string name_no_ext = removeExtension(filename);

//     std::size_t underscore_pos = name_no_ext.find_last_of('_');
//     if (underscore_pos == std::string::npos)
//     {
//         return name_no_ext;
//     }

//     return name_no_ext.substr(0, underscore_pos);
// }

// // ====================== FACE HELPERS ======================

// cv::Mat createFaceLandmarkGTMatrix()
// {
//     float v1[5][2] = {
//         {30.2946f, 51.6963f},
//         {65.5318f, 51.5014f},
//         {48.0252f, 71.7366f},
//         {33.5493f, 92.3655f},
//         {62.7299f, 92.2041f}
//     };

//     cv::Mat src(5, 2, CV_32FC1, v1);
//     memcpy(src.data, v1, 2 * 5 * sizeof(float));
//     return src.clone();
// }

// cv::Mat createFaceLandmarkMatrixfromBBox(const Bbox& box)
// {
//     float v2[5][2] = {
//         {box.ppoint[0], box.ppoint[5]},
//         {box.ppoint[1], box.ppoint[6]},
//         {box.ppoint[2], box.ppoint[7]},
//         {box.ppoint[3], box.ppoint[8]},
//         {box.ppoint[4], box.ppoint[9]},
//     };

//     cv::Mat dst(5, 2, CV_32FC1, v2);
//     memcpy(dst.data, v2, 2 * 5 * sizeof(float));
//     return dst.clone();
// }

// Bbox getLargestBboxFromBboxVec(const std::vector<Bbox>& faces_info)
// {
//     if (faces_info.empty())
//     {
//         return Bbox();
//     }

//     int largest_idx = 0;
//     float largest_area = 0.0f;

//     for (int i = 0; i < (int)faces_info.size(); i++)
//     {
//         float w = faces_info[i].x2 - faces_info[i].x1;
//         float h = faces_info[i].y2 - faces_info[i].y1;
//         float area = w * h;

//         if (area > largest_area)
//         {
//             largest_area = area;
//             largest_idx = i;
//         }
//     }

//     return faces_info[largest_idx];
// }

// static Bbox scaleBbox(const Bbox& box, float sx, float sy)
// {
//     Bbox out = box;

//     out.x1 = box.x1 * sx;
//     out.y1 = box.y1 * sy;
//     out.x2 = box.x2 * sx;
//     out.y2 = box.y2 * sy;

//     for (int i = 0; i < 5; i++)
//     {
//         out.ppoint[i] = box.ppoint[i] * sx;
//         out.ppoint[i + 5] = box.ppoint[i + 5] * sy;
//     }

//     return out;
// }

// LiveFaceBox Bbox2LiveFaceBox(const Bbox& box)
// {
//     LiveFaceBox live_box = {box.x1, box.y1, box.x2, box.y2};
//     return live_box;
// }

// cv::Mat alignFaceImage(
//     const cv::Mat& frame,
//     const Bbox& bbox,
//     const cv::Mat& gt_landmark_matrix
// )
// {
//     cv::Mat face_landmark = createFaceLandmarkMatrixfromBBox(bbox);

//     cv::Mat transf = FacePreprocess::similarTransform(
//         face_landmark,
//         gt_landmark_matrix
//     );

//     cv::Mat aligned = frame.clone();

//     cv::warpPerspective(
//         frame,
//         aligned,
//         transf,
//         cv::Size(96, 112),
//         INTER_LINEAR
//     );

//     cv::resize(
//         aligned,
//         aligned,
//         cv::Size(112, 112),
//         0,
//         0,
//         INTER_LINEAR
//     );

//     return aligned.clone();
// }

// // ====================== DRAW HELPERS ======================

// static void drawTextWithBg(
//     cv::Mat& img,
//     const std::string& text,
//     cv::Point org,
//     double scale,
//     cv::Scalar text_color,
//     cv::Scalar bg_color,
//     int thickness = 2
// )
// {
//     int baseline = 0;

//     cv::Size text_size = cv::getTextSize(
//         text,
//         cv::FONT_HERSHEY_SIMPLEX,
//         scale,
//         thickness,
//         &baseline
//     );

//     cv::Rect bg_rect(
//         org.x - 4,
//         org.y - text_size.height - 6,
//         text_size.width + 8,
//         text_size.height + baseline + 10
//     );

//     bg_rect &= cv::Rect(0, 0, img.cols, img.rows);

//     cv::rectangle(img, bg_rect, bg_color, cv::FILLED);

//     cv::putText(
//         img,
//         text,
//         org,
//         cv::FONT_HERSHEY_SIMPLEX,
//         scale,
//         text_color,
//         thickness
//     );
// }

// static void drawAllDetectedFaces(
//     cv::Mat& frame,
//     const std::vector<Bbox>& faces
// )
// {
//     if (!DRAW_ALL_FACES)
//     {
//         return;
//     }

//     for (const auto& box : faces)
//     {
//         cv::rectangle(
//             frame,
//             cv::Point((int)box.x1, (int)box.y1),
//             cv::Point((int)box.x2, (int)box.y2),
//             cv::Scalar(160, 160, 160),
//             1
//         );
//     }
// }

// static void drawMainFaceResult(
//     cv::Mat& frame,
//     const CachedResult& cached,
//     const PerfInfo& perf
// )
// {
//     const Bbox& box = cached.display_box;
//     const MatchResult& match = cached.match;

//     bool unlock_ok = cached.is_real && match.known;

//     cv::Scalar box_color;
//     cv::Scalar status_bg;

//     if (unlock_ok)
//     {
//         box_color = cv::Scalar(0, 255, 0);
//         status_bg = cv::Scalar(0, 120, 0);
//     }
//     else if (!cached.is_real)
//     {
//         box_color = cv::Scalar(0, 0, 255);
//         status_bg = cv::Scalar(0, 0, 160);
//     }
//     else
//     {
//         box_color = cv::Scalar(0, 255, 255);
//         status_bg = cv::Scalar(0, 120, 120);
//     }

//     cv::rectangle(
//         frame,
//         cv::Point((int)box.x1, (int)box.y1),
//         cv::Point((int)box.x2, (int)box.y2),
//         box_color,
//         3
//     );

//     std::string status_text;

//     if (!cached.is_real)
//     {
//         status_text = "FAKE FACE";
//     }
//     else if (match.known)
//     {
//         status_text = "TRUE FACE";
//     }
//     else
//     {
//         status_text = "TRUE FACE";
//     }

//     std::string fps_text =
//         "FPS: " + fmtDouble(perf.fps, 1);

//     std::string name_text =
//         "Name: " + match.name;

//     int x = std::max(10, (int)box.x1);
//     int y = std::max(30, (int)box.y1 - 15);

//     drawTextWithBg(
//         frame,
//         fps_text,
//         cv::Point(15, 35),
//         0.75,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 0),
//         2
//     );

//     drawTextWithBg(
//         frame,
//         status_text,
//         cv::Point(x, y),
//         0.80,
//         cv::Scalar(255, 255, 255),
//         status_bg,
//         2
//     );

//     drawTextWithBg(
//         frame,
//         name_text,
//         cv::Point(x, y + 38),
//         0.70,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(40, 40, 40),
//         2
//     );

//     drawTextWithBg(
//         frame,
//         "Press q/ESC: quit | Press s: save current face",
//         cv::Point(15, frame.rows - 20),
//         0.55,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 0),
//         1
//     );
// }

// static void drawNoFaceUI(
//     cv::Mat& frame,
//     const PerfInfo& perf
// )
// {
//     drawTextWithBg(
//         frame,
//         "FPS: " + fmtDouble(perf.fps, 1),
//         cv::Point(15, 35),
//         0.75,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 0),
//         2
//     );

//     drawTextWithBg(
//         frame,
//         "No face detected",
//         cv::Point(15, 75),
//         0.8,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 160),
//         2
//     );

//     drawTextWithBg(
//         frame,
//         "Press q/ESC: quit",
//         cv::Point(15, frame.rows - 20),
//         0.55,
//         cv::Scalar(255, 255, 255),
//         cv::Scalar(0, 0, 0),
//         1
//     );
// }

// // ====================== MODEL / USER DB ======================

// void loadLiveModel(Live& live)
// {
//     ModelConfig config1 = {
//         2.7f,
//         0.0f,
//         0.0f,
//         80,
//         80,
//         "model_1",
//         false
//     };

//     ModelConfig config2 = {
//         4.0f,
//         0.0f,
//         0.0f,
//         80,
//         80,
//         "model_2",
//         false
//     };

//     vector<ModelConfig> configs;
//     configs.emplace_back(config1);
//     configs.emplace_back(config2);

//     live.LoadModel(configs);
// }

// static cv::Mat prepareUserImageForFeature(
//     const cv::Mat& input_img,
//     const cv::Mat& gt_landmark_matrix
// )
// {
//     if (input_img.empty())
//     {
//         return cv::Mat();
//     }

//     if (input_img.cols == 112 && input_img.rows == 112)
//     {
//         return input_img.clone();
//     }

//     std::vector<Bbox> faces = detect_mtcnn(input_img);

//     if (!faces.empty())
//     {
//         Bbox largest = getLargestBboxFromBboxVec(faces);
//         return alignFaceImage(input_img, largest, gt_landmark_matrix);
//     }

//     cv::Mat resized;
//     cv::resize(
//         input_img,
//         resized,
//         cv::Size(112, 112),
//         0,
//         0,
//         INTER_LINEAR
//     );

//     return resized;
// }

// static void loadRegisteredUsers(
//     Arcface& facereco,
//     std::map<std::string, std::vector<cv::Mat>>& user_descriptors,
//     const cv::Mat& gt_landmark_matrix
// )
// {
//     ensureDir(USER_DIR);

//     std::vector<cv::String> image_names;
//     std::vector<cv::String> jpg_names;
//     std::vector<cv::String> jpeg_names;
//     std::vector<cv::String> png_names;

//     cv::glob(USER_DIR + "/*.jpg", jpg_names, false);
//     cv::glob(USER_DIR + "/*.jpeg", jpeg_names, false);
//     cv::glob(USER_DIR + "/*.png", png_names, false);

//     image_names.insert(image_names.end(), jpg_names.begin(), jpg_names.end());
//     image_names.insert(image_names.end(), jpeg_names.begin(), jpeg_names.end());
//     image_names.insert(image_names.end(), png_names.begin(), png_names.end());

//     if (image_names.empty())
//     {
//         std::cerr << "[ERROR] No image files found in: "
//                   << USER_DIR << std::endl;
//         std::cerr << "[HINT] Put registered face images into users/ folder."
//                   << std::endl;
//         std::cerr << "[HINT] Example: users/Cong_001.jpg, users/Duc_001.jpg"
//                   << std::endl;
//         exit(0);
//     }

//     std::cout << "[INFO] Loading registered users from: "
//               << USER_DIR << std::endl;
//     std::cout << "[INFO] Total user images: "
//               << image_names.size() << std::endl;

//     int loaded = 0;
//     int skipped = 0;

//     for (const auto& img_name_cv : image_names)
//     {
//         std::string img_name = std::string(img_name_cv);
//         std::string person_name = extractPersonName(img_name);

//         cv::Mat img = cv::imread(img_name);

//         if (img.empty())
//         {
//             std::cerr << "[WARN] Cannot read image: "
//                       << img_name << std::endl;
//             skipped++;
//             continue;
//         }

//         cv::Mat face_img = prepareUserImageForFeature(
//             img,
//             gt_landmark_matrix
//         );

//         if (face_img.empty())
//         {
//             std::cerr << "[WARN] Cannot prepare face image: "
//                       << img_name << std::endl;
//             skipped++;
//             continue;
//         }

//         cv::Mat descriptor = facereco.getFeature(face_img);
//         descriptor = Statistics::zScore(descriptor);

//         user_descriptors[person_name].push_back(descriptor);
//         loaded++;

//         std::cout << "[LOAD] "
//                   << person_name
//                   << " <= "
//                   << basenameFromPath(img_name)
//                   << std::endl;
//     }

//     std::cout << "[INFO] Loaded descriptors: "
//               << loaded << std::endl;
//     std::cout << "[INFO] Skipped images: "
//               << skipped << std::endl;
//     std::cout << "[INFO] Registered people: "
//               << user_descriptors.size() << std::endl;

//     if (user_descriptors.empty())
//     {
//         std::cerr << "[ERROR] No valid user descriptor loaded."
//                   << std::endl;
//         exit(0);
//     }
// }

// static MatchResult findBestMatch(
//     const std::map<std::string, std::vector<cv::Mat>>& user_descriptors,
//     const cv::Mat& face_descriptor
// )
// {
//     MatchResult result;
//     result.name = "Unknown";
//     result.score = -1.0;
//     result.known = false;

//     for (const auto& user_pair : user_descriptors)
//     {
//         const std::string& name = user_pair.first;
//         const std::vector<cv::Mat>& descriptors = user_pair.second;

//         for (const auto& ref_desc : descriptors)
//         {
//             double score = Statistics::cosineDistance(
//                 ref_desc,
//                 face_descriptor
//             );

//             if (score > result.score)
//             {
//                 result.score = score;
//                 result.name = name;
//             }
//         }
//     }

//     if (result.score > face_thre)
//     {
//         result.known = true;
//     }
//     else
//     {
//         result.name = "Unknown";
//         result.known = false;
//     }

//     return result;
// }

// static std::string makeSaveFileName(const std::string& name)
// {
//     auto now = std::chrono::system_clock::now();

//     long long ms =
//         std::chrono::duration_cast<std::chrono::milliseconds>(
//             now.time_since_epoch()
//         ).count();

//     return USER_DIR + "/" + name + "_" + std::to_string(ms) + ".jpg";
// }

// // ====================== MAIN PIPELINE ======================

// int MTCNNDetection()
// {
//     std::cout << "OpenCV Version: "
//               << CV_MAJOR_VERSION << "."
//               << CV_MINOR_VERSION << "."
//               << CV_SUBMINOR_VERSION << std::endl;

//     std::cout << "[INFO] project_path = "
//               << project_path << std::endl;
//     std::cout << "[INFO] USER_DIR     = "
//               << USER_DIR << std::endl;
//     std::cout << "[INFO] CSI CAMERA   = "
//               << CAMERA_WIDTH << "x" << CAMERA_HEIGHT
//               << "@" << CAMERA_FPS << std::endl;
//     std::cout << "[INFO] PROCESS SIZE = "
//               << PROCESS_WIDTH << "x" << PROCESS_HEIGHT << std::endl;
//     std::cout << "[INFO] PROCESS STEP = every "
//               << PROCESS_EVERY_N_FRAMES << " frames" << std::endl;
//     std::cout << "[INFO] face_thre    = "
//               << face_thre << std::endl;
//     std::cout << "[INFO] true_thre    = "
//               << true_thre << std::endl;

//     ensureDir(USER_DIR);

//     cv::Mat face_landmark_gt_matrix = createFaceLandmarkGTMatrix();

//     Arcface facereco;

//     std::map<std::string, std::vector<cv::Mat>> user_descriptors;

//     loadRegisteredUsers(
//         facereco,
//         user_descriptors,
//         face_landmark_gt_matrix
//     );

//     Live live;
//     loadLiveModel(live);

//     RpiCamLatestCapture cap(CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS);

//     if (!cap.open())
//     {
//         std::cerr << "[ERROR] Cannot open CSI camera using rpicam-vid."
//                   << std::endl;
//         std::cerr << "[HINT] Test camera first:" << std::endl;
//         std::cerr << "       rpicam-hello --list-cameras" << std::endl;
//         std::cerr << "       rpicam-hello" << std::endl;
//         std::cerr << "[HINT] Check log:" << std::endl;
//         std::cerr << "       cat " << RPICAM_LOG << std::endl;
//         return -1;
//     }

//     cv::namedWindow("LiveFaceReco - Smart Lock", cv::WINDOW_NORMAL);
//     cv::resizeWindow(
//         "LiveFaceReco - Smart Lock",
//         DISPLAY_WIDTH,
//         DISPLAY_HEIGHT
//     );

//     cv::Mat frame;
//     cv::Mat process_frame;
//     cv::Mat display_frame;

//     CachedResult cached;

//     int frame_count = 0;

//     double last_infer_ms = 0.0;
//     double last_loop_ms = 0.0;
//     double instant_fps = 0.0;

//     uint64_t camera_frame_id = 0;

//     PerfInfo perf;

//     std::cout << "[INFO] CSI camera started." << std::endl;
//     std::cout << "[INFO] Capture mode: latest-frame only, no stale pipe backlog." << std::endl;
//     std::cout << "[INFO] GUI: FPS + TRUE/FAKE/NO FACE + Name only." << std::endl;
//     std::cout << "[INFO] Terminal: FPS + loop_ms + infer_ms + rec_score + live_score + status + name." << std::endl;
//     std::cout << "[INFO] Press q or ESC to quit." << std::endl;
//     std::cout << "[INFO] Press s to save current aligned face into users/."
//               << std::endl;

//     while (cap.isOpened())
//     {
//         auto loop_start = std::chrono::steady_clock::now();

//         bool got_frame = cap.getFrame(frame, camera_frame_id);

//         if (!got_frame || frame.empty())
//         {
//             continue;
//         }

//         frame_count++;

//         display_frame = frame.clone();

//         bool should_process =
//             (frame_count % PROCESS_EVERY_N_FRAMES == 0) ||
//             (frame_count == 1);

//         if (should_process)
//         {
//             auto infer_start = std::chrono::steady_clock::now();

//             cv::resize(
//                 frame,
//                 process_frame,
//                 cv::Size(PROCESS_WIDTH, PROCESS_HEIGHT),
//                 0,
//                 0,
//                 INTER_LINEAR
//             );

//             float sx = static_cast<float>(frame.cols) / PROCESS_WIDTH;
//             float sy = static_cast<float>(frame.rows) / PROCESS_HEIGHT;

//             std::vector<Bbox> faces_info = detect_mtcnn(process_frame);

//             if (!faces_info.empty())
//             {
//                 Bbox largest_process_box =
//                     getLargestBboxFromBboxVec(faces_info);

//                 Bbox largest_display_box =
//                     scaleBbox(largest_process_box, sx, sy);

//                 LiveFaceBox live_face_box =
//                     Bbox2LiveFaceBox(largest_process_box);

//                 cv::Mat aligned_img = alignFaceImage(
//                     process_frame,
//                     largest_process_box,
//                     face_landmark_gt_matrix
//                 );

//                 cv::Mat face_descriptor = facereco.getFeature(aligned_img);
//                 face_descriptor = Statistics::zScore(face_descriptor);

//                 MatchResult match = findBestMatch(
//                     user_descriptors,
//                     face_descriptor
//                 );

//                 double live_score =
//                     live.Detect(process_frame, live_face_box);

//                 live_score = clamp01(live_score);
//                 bool is_real = live_score > true_thre;

//                 cached.has_face = true;
//                 cached.has_aligned_face = true;
//                 cached.match = match;
//                 cached.live_score = live_score;
//                 cached.is_real = is_real;
//                 cached.process_box = largest_process_box;
//                 cached.display_box = largest_display_box;

//                 // Pipeline chỉ dùng duy nhất bbox lớn nhất.
//                 cached.face_count = 1;

//                 cached.aligned_face = aligned_img.clone();

//                 if (DRAW_ALL_FACES)
//                 {
//                     std::vector<Bbox> scaled_faces;
//                     for (const auto& fb : faces_info)
//                     {
//                         scaled_faces.push_back(scaleBbox(fb, sx, sy));
//                     }
//                     drawAllDetectedFaces(display_frame, scaled_faces);
//                 }
//             }
//             else
//             {
//                 cached.has_face = false;
//                 cached.has_aligned_face = false;
//                 cached.match = {"Unknown", -1.0, false};
//                 cached.live_score = 0.0;
//                 cached.is_real = false;
//                 cached.face_count = 0;
//                 cached.aligned_face.release();
//             }

//             auto infer_end = std::chrono::steady_clock::now();

//             last_infer_ms =
//                 std::chrono::duration<double, std::milli>(
//                     infer_end - infer_start
//                 ).count();

//             cached.last_infer_ms = last_infer_ms;
//         }

//         // Tính metric trước khi vẽ GUI để GUI và terminal dùng cùng số.
//         auto metric_time = std::chrono::steady_clock::now();

//         double loop_dt = std::chrono::duration<double>(
//             metric_time - loop_start
//         ).count();

//         if (loop_dt > 0.000001)
//         {
//             last_loop_ms = loop_dt * 1000.0;
//             instant_fps = 1.0 / loop_dt;
//         }
//         else
//         {
//             last_loop_ms = 0.0;
//             instant_fps = 0.0;
//         }

//         perf.fps = instant_fps;
//         perf.loop_ms = last_loop_ms;
//         perf.infer_ms = last_infer_ms;

//         if (cached.has_face)
//         {
//             drawMainFaceResult(display_frame, cached, perf);
//         }
//         else
//         {
//             drawNoFaceUI(display_frame, perf);
//         }

//         cv::imshow("LiveFaceReco - Smart Lock", display_frame);

//         int key = cv::waitKey(1);

//         if (key == 27 || key == 'q' || key == 'Q')
//         {
//             break;
//         }

//         if (key == 's' || key == 'S')
//         {
//             if (cached.has_aligned_face && !cached.aligned_face.empty())
//             {
//                 std::string save_path = makeSaveFileName(SAVE_NAME);

//                 bool ok = cv::imwrite(save_path, cached.aligned_face);

//                 if (ok)
//                 {
//                     cv::Mat desc = facereco.getFeature(cached.aligned_face);
//                     desc = Statistics::zScore(desc);
//                     user_descriptors[SAVE_NAME].push_back(desc);

//                     std::cout << "[SAVE] Saved new face: "
//                               << save_path << std::endl;
//                     std::cout << "[SAVE] Added descriptor to current database."
//                               << std::endl;
//                 }
//                 else
//                 {
//                     std::cerr << "[ERROR] Cannot save image: "
//                               << save_path << std::endl;
//                 }
//             }
//             else
//             {
//                 std::cout << "[WARN] No current face to save."
//                           << std::endl;
//             }
//         }

//         if (frame_count % LOG_EVERY_N_FRAMES == 0)
//         {
//             std::cout << "[FRAME " << frame_count << "] ";

//             if (cached.has_face)
//             {
//                 std::string status_text = cached.is_real ? "TRUE" : "FAKE";

//                 std::cout << "fps="
//                           << fmtDouble(perf.fps, 1)
//                           << " | loop_ms="
//                           << fmtDouble(perf.loop_ms, 1)
//                           << " | infer_ms="
//                           << fmtDouble(perf.infer_ms, 1)
//                           << " | face=yes"
//                           << " | status="
//                           << status_text
//                           << " | name="
//                           << cached.match.name
//                           << " | known="
//                           << (cached.match.known ? "yes" : "no")
//                           << " | rec_score="
//                           << fmtDouble(cached.match.score, 3)
//                           << " | live_score="
//                           << fmtDouble(cached.live_score, 3)
//                           << std::endl;
//             }
//             else
//             {
//                 std::cout << "fps="
//                           << fmtDouble(perf.fps, 1)
//                           << " | loop_ms="
//                           << fmtDouble(perf.loop_ms, 1)
//                           << " | infer_ms="
//                           << fmtDouble(perf.infer_ms, 1)
//                           << " | face=no"
//                           << " | status=NO_FACE"
//                           << " | name=Unknown"
//                           << " | known=no"
//                           << " | rec_score=-1.000"
//                           << " | live_score=0.000"
//                           << std::endl;
//             }
//         }
//     }

//     cap.close();
//     cv::destroyAllWindows();

//     std::cout << "[INFO] CSI camera stopped." << std::endl;

//     return 0;
// }




















//
// Optimized Realtime Live Face Recognition GUI for Raspberry Pi 4 + CSI OV5647
// - Capture CSI camera using rpicam-vid YUV420 pipe
// - Load registered users from users/ folder
// - Process inference on resized frame for better FPS
// - Main GUI shows only: name, real/fake status, live score, recognition score
// - Main GUI has only one button: ADD USER
// - ADD USER opens a separate "Add User" window
// - Add User window has INPUT NAME, CAPTURE, SAVE, EXIT buttons
// - SAVE is enabled only after 10 valid REAL face images are captured
// - EXIT discards unsaved captures if fewer than 10 images were saved
// - Terminal log shows: name, real/fake, live score, recog score, FPS, loop time,
//   total inference time, and per-model AI time: detect / recognition / anti-spoof
// - Gamma Correction + Laplacian Sharpening are applied only for face recognition branch
// - Press q to quit main window
//

#include <math.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <vector>

#include "livefacereco.hpp"
#include "math.hpp"
#include "mtcnn_new.h"
#include "FacePreprocess.h"
// #include "DatasetHandler/image_dataset_handler.hpp"

#define PI 3.14159265

using namespace std;
using namespace cv;

// ====================== CONFIG ======================
//
// Nhớ sửa project_path trong:
// 1. src/livefacereco.hpp
// 2. src/arcface.h
//
// Ví dụ:
// const string project_path="/home/pi4/LiveFaceReco_RaspberryPi";
//
// ====================================================

static const std::string USER_DIR = project_path + "/users";

// Camera CSI OV5647 capture size.
static const int CAMERA_WIDTH  = 640;
static const int CAMERA_HEIGHT = 480;
static const int CAMERA_FPS    = 30;

// Kích thước đưa vào MTCNN/ArcFace/Anti-spoof.
static const int PROCESS_WIDTH  = 320;
static const int PROCESS_HEIGHT = 240;

// Chỉ chạy full model mỗi N frame.
// Nếu muốn kết quả realtime mượt và đúng thời điểm hơn, để 1.
// Nếu muốn nhẹ hơn cho Pi 4, để 2 hoặc 3.
static const int PROCESS_EVERY_N_FRAMES = 3;

// Mỗi người mới cần chụp đủ 10 ảnh hợp lệ.
static const int REGISTER_REQUIRED_IMAGES = 10;

// GUI size
static const int DISPLAY_WIDTH  = 960;
static const int DISPLAY_HEIGHT = 720;

// Vẽ tất cả bbox detect được
static const bool DRAW_ALL_FACES = false;

// In log terminal mỗi N frame
static const int LOG_EVERY_N_FRAMES = 15;

// rpicam stderr log
static const std::string RPICAM_LOG = "/tmp/rpicam_livefacereco.log";

// ====================== PREPROCESS CONFIG ======================
//
// Chỉ áp dụng preprocess cho ảnh face đã align trước khi đưa vào ArcFace.
// Không áp dụng cho MTCNN và anti-spoofing để tránh làm lệch input của model.
//

static const bool USE_FACE_PREPROCESSING = true;

// Gamma correction:
// gamma < 1.0  => làm ảnh sáng hơn
// gamma = 1.0  => giữ nguyên
// gamma > 1.0  => làm ảnh tối hơn
static const bool USE_GAMMA_CORRECTION = true;
static const double GAMMA_VALUE = 0.75;

// Laplacian sharpening:
// alpha càng lớn thì ảnh càng sắc, nhưng dễ nhiễu.
// Khuyên dùng 0.10 -> 0.30 trên Pi/camera thật.
static const bool USE_LAPLACE_SHARPEN = true;
static const double LAPLACE_ALPHA = 0.20;

// ====================================================

double sum_score, sum_fps, sum_confidence;

struct MatchResult
{
    std::string name;
    double score;
    bool known;
};

struct PerfStats
{
    double camera_ms = 0.0;
    double resize_ms = 0.0;
    double detect_ms = 0.0;
    double align_pre_ms = 0.0;
    double recog_ms = 0.0;
    double match_ms = 0.0;
    double anti_ms = 0.0;
    double infer_ms = 0.0;
    double loop_ms = 0.0;
    double fps_real = 0.0;
};

struct CachedResult
{
    bool has_face = false;
    bool has_aligned_face = false;
    MatchResult match = {"Unknown", -1.0, false};
    double live_score = 0.0;
    bool is_real = false;
    Bbox process_box;
    Bbox display_box;
    int face_count = 0;

    // Lưu ảnh face đã align gốc.
    // Khi tính descriptor sẽ gọi preprocessFaceForRecognition().
    // Làm vậy để tránh save ảnh đã preprocess rồi lần sau lại preprocess thêm lần nữa.
    cv::Mat aligned_face;

    int result_frame_id = 0;
    PerfStats perf;
};


struct AddUserState
{
    bool window_open = false;
    bool input_active = false;
    std::string input_name;
    std::string safe_name;
    std::vector<cv::Mat> captured_faces;
    bool saved_current_batch = false;
    std::string message = "Click INPUT NAME, type name, then capture 10 REAL faces.";
};

// ====================== SIMPLE GUI BUTTON STATE ======================

static const std::string MAIN_WINDOW_NAME = "LiveFaceReco - Smart Lock";
static const std::string ADD_USER_WINDOW_NAME = "Add User";

static cv::Rect g_main_add_button_rect;

static cv::Rect g_add_input_button_rect;
static cv::Rect g_add_capture_button_rect;
static cv::Rect g_add_save_button_rect;
static cv::Rect g_add_exit_button_rect;

static bool g_main_add_clicked = false;
static bool g_add_input_clicked = false;
static bool g_add_capture_clicked = false;
static bool g_add_save_clicked = false;
static bool g_add_exit_clicked = false;

static void onMainMouseEvent(int event, int x, int y, int flags, void* userdata)
{
    (void)flags;
    (void)userdata;

    if (event != cv::EVENT_LBUTTONDOWN)
    {
        return;
    }

    if (g_main_add_button_rect.contains(cv::Point(x, y)))
    {
        g_main_add_clicked = true;
    }
}

static void onAddUserMouseEvent(int event, int x, int y, int flags, void* userdata)
{
    (void)flags;
    (void)userdata;

    if (event != cv::EVENT_LBUTTONDOWN)
    {
        return;
    }

    cv::Point p(x, y);

    if (g_add_input_button_rect.contains(p))
    {
        g_add_input_clicked = true;
    }
    else if (g_add_capture_button_rect.contains(p))
    {
        g_add_capture_clicked = true;
    }
    else if (g_add_save_button_rect.contains(p))
    {
        g_add_save_clicked = true;
    }
    else if (g_add_exit_button_rect.contains(p))
    {
        g_add_exit_clicked = true;
    }
}

// ====================== TIMER HELPER ======================

static double elapsedMs(
    const std::chrono::steady_clock::time_point& start,
    const std::chrono::steady_clock::time_point& end
)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// ====================== RPICAM CAPTURE ======================

class RpiCamRawCapture
{
public:
    RpiCamRawCapture(int width, int height, int fps)
        : width_(width),
          height_(height),
          fps_(fps),
          frame_size_(static_cast<size_t>(width) * height * 3 / 2),
          pipe_(nullptr),
          opened_(false)
    {
    }

    bool open()
    {
        std::ostringstream cmd;

        cmd << "rpicam-vid "
            << "-t 0 "
            << "--width " << width_ << " "
            << "--height " << height_ << " "
            << "--framerate " << fps_ << " "
            << "--codec yuv420 "
            << "-n "
            << "-o - "
            << "2>" << RPICAM_LOG;

        std::cout << "[INFO] Starting CSI camera with command:" << std::endl;
        std::cout << "[INFO] " << cmd.str() << std::endl;

        pipe_ = popen(cmd.str().c_str(), "r");

        if (!pipe_)
        {
            std::cerr << "[ERROR] Cannot start rpicam-vid." << std::endl;
            opened_ = false;
            return false;
        }

        buffer_.resize(frame_size_);
        opened_ = true;
        return true;
    }

    bool isOpened() const
    {
        return opened_ && pipe_ != nullptr;
    }

    cv::Mat getFrame()
    {
        if (!isOpened())
        {
            return cv::Mat();
        }

        size_t total_read = 0;

        while (total_read < frame_size_)
        {
            size_t n = fread(
                buffer_.data() + total_read,
                1,
                frame_size_ - total_read,
                pipe_
            );

            if (n == 0)
            {
                std::cerr << "[ERROR] rpicam stream ended or read failed." << std::endl;
                std::cerr << "[HINT] Check log: cat " << RPICAM_LOG << std::endl;
                close();
                return cv::Mat();
            }

            total_read += n;
        }

        cv::Mat yuv(
            height_ * 3 / 2,
            width_,
            CV_8UC1,
            buffer_.data()
        );

        cv::Mat bgr;
        cv::cvtColor(yuv, bgr, cv::COLOR_YUV2BGR_I420);

        return bgr.clone();
    }

    void close()
    {
        if (pipe_)
        {
            pclose(pipe_);
            pipe_ = nullptr;
        }

        opened_ = false;
    }

    ~RpiCamRawCapture()
    {
        close();
    }

private:
    int width_;
    int height_;
    int fps_;
    size_t frame_size_;
    FILE* pipe_;
    bool opened_;
    std::vector<unsigned char> buffer_;
};

// ====================== UTILS ======================

static void ensureDir(const std::string& path)
{
    mkdir(path.c_str(), 0755);
}

static double clamp01(double v)
{
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static std::string fmtDouble(double v, int precision = 2)
{
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(precision) << v;
    return oss.str();
}

static std::string makeSafeName(const std::string& raw)
{
    std::string out;

    for (char c : raw)
    {
        unsigned char uc = static_cast<unsigned char>(c);

        if (std::isalnum(uc) || c == '_' || c == '-')
        {
            out.push_back(c);
        }
    }

    return out;
}

// ====================== GAMMA + LAPLACE PREPROCESS ======================

static cv::Mat applyGammaCorrection(const cv::Mat& src, double gamma)
{
    if (src.empty())
    {
        return src.clone();
    }

    if (gamma <= 0.0)
    {
        std::cerr << "[WARN] Invalid gamma value. Skip gamma correction." << std::endl;
        return src.clone();
    }

    cv::Mat lut(1, 256, CV_8UC1);
    uchar* p = lut.ptr();

    for (int i = 0; i < 256; i++)
    {
        double normalized = static_cast<double>(i) / 255.0;
        double corrected = std::pow(normalized, gamma) * 255.0;
        p[i] = cv::saturate_cast<uchar>(corrected);
    }

    cv::Mat dst;
    cv::LUT(src, lut, dst);

    return dst;
}

static cv::Mat applyLaplacianSharpen(const cv::Mat& src, double alpha)
{
    if (src.empty())
    {
        return src.clone();
    }

    if (alpha <= 0.0)
    {
        return src.clone();
    }

    cv::Mat src_float;
    cv::Mat blur_float;
    cv::Mat laplace;
    cv::Mat sharp_float;
    cv::Mat dst;

    src.convertTo(src_float, CV_32F);

    cv::GaussianBlur(
        src_float,
        blur_float,
        cv::Size(3, 3),
        0
    );

    cv::Laplacian(
        blur_float,
        laplace,
        CV_32F,
        3
    );

    sharp_float = src_float - alpha * laplace;
    sharp_float.convertTo(dst, CV_8U);

    return dst;
}

static cv::Mat preprocessFaceForRecognition(const cv::Mat& face)
{
    if (face.empty())
    {
        return face.clone();
    }

    if (!USE_FACE_PREPROCESSING)
    {
        return face.clone();
    }

    cv::Mat out = face.clone();

    if (USE_GAMMA_CORRECTION)
    {
        out = applyGammaCorrection(out, GAMMA_VALUE);
    }

    if (USE_LAPLACE_SHARPEN)
    {
        out = applyLaplacianSharpen(out, LAPLACE_ALPHA);
    }

    return out;
}

// ====================== STRING HELPERS ======================

std::vector<std::string> split(const std::string& s, char seperator)
{
    std::vector<std::string> output;
    std::string::size_type prev_pos = 0, pos = 0;

    while ((pos = s.find(seperator, pos)) != std::string::npos)
    {
        std::string substring(s.substr(prev_pos, pos - prev_pos));
        output.push_back(substring);
        prev_pos = ++pos;
    }

    output.push_back(s.substr(prev_pos, pos - prev_pos));
    return output;
}

static std::string basenameFromPath(const std::string& path)
{
    std::vector<std::string> parts = split(path, '/');
    if (parts.empty()) return path;
    return parts.back();
}

static std::string removeExtension(const std::string& filename)
{
    std::size_t dot_pos = filename.find_last_of('.');
    if (dot_pos == std::string::npos) return filename;
    return filename.substr(0, dot_pos);
}

static std::string extractPersonName(const std::string& img_path)
{
    std::string filename = basenameFromPath(img_path);
    std::string name_no_ext = removeExtension(filename);

    std::size_t underscore_pos = name_no_ext.find_last_of('_');
    if (underscore_pos == std::string::npos)
    {
        return name_no_ext;
    }

    return name_no_ext.substr(0, underscore_pos);
}

// ====================== FACE HELPERS ======================

cv::Mat createFaceLandmarkGTMatrix()
{
    float v1[5][2] = {
        {30.2946f, 51.6963f},
        {65.5318f, 51.5014f},
        {48.0252f, 71.7366f},
        {33.5493f, 92.3655f},
        {62.7299f, 92.2041f}
    };

    cv::Mat src(5, 2, CV_32FC1, v1);
    memcpy(src.data, v1, 2 * 5 * sizeof(float));
    return src.clone();
}

cv::Mat createFaceLandmarkMatrixfromBBox(const Bbox& box)
{
    float v2[5][2] = {
        {box.ppoint[0], box.ppoint[5]},
        {box.ppoint[1], box.ppoint[6]},
        {box.ppoint[2], box.ppoint[7]},
        {box.ppoint[3], box.ppoint[8]},
        {box.ppoint[4], box.ppoint[9]},
    };

    cv::Mat dst(5, 2, CV_32FC1, v2);
    memcpy(dst.data, v2, 2 * 5 * sizeof(float));
    return dst.clone();
}

Bbox getLargestBboxFromBboxVec(const std::vector<Bbox>& faces_info)
{
    if (faces_info.empty())
    {
        return Bbox();
    }

    int largest_idx = 0;
    float largest_area = 0.0f;

    for (int i = 0; i < (int)faces_info.size(); i++)
    {
        float w = faces_info[i].x2 - faces_info[i].x1;
        float h = faces_info[i].y2 - faces_info[i].y1;
        float area = w * h;

        if (area > largest_area)
        {
            largest_area = area;
            largest_idx = i;
        }
    }

    return faces_info[largest_idx];
}

static Bbox scaleBbox(const Bbox& box, float sx, float sy)
{
    Bbox out = box;

    out.x1 = box.x1 * sx;
    out.y1 = box.y1 * sy;
    out.x2 = box.x2 * sx;
    out.y2 = box.y2 * sy;

    for (int i = 0; i < 5; i++)
    {
        out.ppoint[i] = box.ppoint[i] * sx;
        out.ppoint[i + 5] = box.ppoint[i + 5] * sy;
    }

    return out;
}

LiveFaceBox Bbox2LiveFaceBox(const Bbox& box)
{
    LiveFaceBox live_box = {box.x1, box.y1, box.x2, box.y2};
    return live_box;
}

cv::Mat alignFaceImage(
    const cv::Mat& frame,
    const Bbox& bbox,
    const cv::Mat& gt_landmark_matrix
)
{
    cv::Mat face_landmark = createFaceLandmarkMatrixfromBBox(bbox);

    cv::Mat transf = FacePreprocess::similarTransform(
        face_landmark,
        gt_landmark_matrix
    );

    cv::Mat aligned = frame.clone();

    cv::warpPerspective(
        frame,
        aligned,
        transf,
        cv::Size(96, 112),
        INTER_LINEAR
    );

    cv::resize(
        aligned,
        aligned,
        cv::Size(112, 112),
        0,
        0,
        INTER_LINEAR
    );

    return aligned.clone();
}

// ====================== DRAW HELPERS ======================

static void drawTextWithBg(
    cv::Mat& img,
    const std::string& text,
    cv::Point org,
    double scale,
    cv::Scalar text_color,
    cv::Scalar bg_color,
    int thickness = 2
)
{
    int baseline = 0;

    cv::Size text_size = cv::getTextSize(
        text,
        cv::FONT_HERSHEY_SIMPLEX,
        scale,
        thickness,
        &baseline
    );

    cv::Rect bg_rect(
        org.x - 4,
        org.y - text_size.height - 6,
        text_size.width + 8,
        text_size.height + baseline + 10
    );

    bg_rect &= cv::Rect(0, 0, img.cols, img.rows);

    cv::rectangle(img, bg_rect, bg_color, cv::FILLED);

    cv::putText(
        img,
        text,
        org,
        cv::FONT_HERSHEY_SIMPLEX,
        scale,
        text_color,
        thickness
    );
}

static void drawButton(
    cv::Mat& frame,
    cv::Rect rect,
    const std::string& text,
    cv::Scalar bg_color
)
{
    cv::rectangle(frame, rect, bg_color, cv::FILLED);
    cv::rectangle(frame, rect, cv::Scalar(255, 255, 255), 1);

    int baseline = 0;
    cv::Size text_size = cv::getTextSize(
        text,
        cv::FONT_HERSHEY_SIMPLEX,
        0.55,
        2,
        &baseline
    );

    int x = rect.x + (rect.width - text_size.width) / 2;
    int y = rect.y + (rect.height + text_size.height) / 2;

    cv::putText(
        frame,
        text,
        cv::Point(x, y),
        cv::FONT_HERSHEY_SIMPLEX,
        0.55,
        cv::Scalar(255, 255, 255),
        2
    );
}

static void drawAllDetectedFaces(
    cv::Mat& frame,
    const std::vector<Bbox>& faces
)
{
    for (const auto& box : faces)
    {
        cv::rectangle(
            frame,
            cv::Point((int)box.x1, (int)box.y1),
            cv::Point((int)box.x2, (int)box.y2),
            cv::Scalar(160, 160, 160),
            1
        );
    }
}

static void drawMainFaceResult(
    cv::Mat& frame,
    const CachedResult& cached
)
{
    const Bbox& box = cached.display_box;
    const MatchResult& match = cached.match;

    cv::Scalar box_color;
    cv::Scalar status_bg;

    if (cached.is_real && match.known)
    {
        box_color = cv::Scalar(0, 255, 0);
        status_bg = cv::Scalar(0, 120, 0);
    }
    else if (!cached.is_real)
    {
        box_color = cv::Scalar(0, 0, 255);
        status_bg = cv::Scalar(0, 0, 160);
    }
    else
    {
        box_color = cv::Scalar(0, 255, 255);
        status_bg = cv::Scalar(0, 120, 120);
    }

    cv::rectangle(
        frame,
        cv::Point((int)box.x1, (int)box.y1),
        cv::Point((int)box.x2, (int)box.y2),
        box_color,
        3
    );

    std::string status_text = cached.is_real ? "REAL FACE" : "FAKE FACE";
    std::string name_text = "Name: " + match.name;
    std::string live_text = "Live score: " + fmtDouble(cached.live_score, 3);
    std::string rec_text = "Rec score: " + fmtDouble(match.score, 3);

    int x = std::max(10, (int)box.x1);
    int y = std::max(30, (int)box.y1 - 15);

    drawTextWithBg(
        frame,
        name_text,
        cv::Point(x, y),
        0.70,
        cv::Scalar(255, 255, 255),
        cv::Scalar(40, 40, 40),
        2
    );

    drawTextWithBg(
        frame,
        status_text,
        cv::Point(x, y + 35),
        0.70,
        cv::Scalar(255, 255, 255),
        status_bg,
        2
    );

    drawTextWithBg(
        frame,
        live_text,
        cv::Point(x, y + 68),
        0.55,
        cv::Scalar(255, 255, 255),
        cv::Scalar(40, 40, 40),
        1
    );

    drawTextWithBg(
        frame,
        rec_text,
        cv::Point(x, y + 95),
        0.55,
        cv::Scalar(255, 255, 255),
        cv::Scalar(40, 40, 40),
        1
    );
}

static void drawNoFaceUI(cv::Mat& frame)
{
    drawTextWithBg(
        frame,
        "No face detected",
        cv::Point(15, 35),
        0.8,
        cv::Scalar(255, 255, 255),
        cv::Scalar(0, 0, 160),
        2
    );
}


static void drawMainBottomBar(cv::Mat& frame)
{
    const int panel_h = 70;
    cv::Rect panel(0, frame.rows - panel_h, frame.cols, panel_h);
    cv::rectangle(frame, panel, cv::Scalar(20, 20, 20), cv::FILLED);

    g_main_add_button_rect = cv::Rect(20, frame.rows - 55, 150, 40);
    drawButton(frame, g_main_add_button_rect, "ADD USER", cv::Scalar(55, 55, 55));

    cv::putText(
        frame,
        "Q: quit",
        cv::Point(190, frame.rows - 28),
        cv::FONT_HERSHEY_SIMPLEX,
        0.55,
        cv::Scalar(220, 220, 220),
        1
    );
}

static void drawAddUserWindow(
    AddUserState& add_state,
    const CachedResult& cached
)
{
    cv::Mat win(420, 640, CV_8UC3, cv::Scalar(28, 28, 28));

    cv::putText(
        win,
        "ADD USER",
        cv::Point(25, 40),
        cv::FONT_HERSHEY_SIMPLEX,
        0.95,
        cv::Scalar(255, 255, 255),
        2
    );

    cv::putText(
        win,
        "Name: " + add_state.input_name + (add_state.input_active ? "_" : ""),
        cv::Point(25, 88),
        cv::FONT_HERSHEY_SIMPLEX,
        0.68,
        cv::Scalar(255, 255, 255),
        2
    );

    cv::putText(
        win,
        "Captured: " + std::to_string((int)add_state.captured_faces.size()) +
            "/" + std::to_string(REGISTER_REQUIRED_IMAGES),
        cv::Point(25, 125),
        cv::FONT_HERSHEY_SIMPLEX,
        0.65,
        cv::Scalar(230, 230, 230),
        2
    );

    std::string face_line;
    cv::Scalar face_color;

    if (!cached.has_face)
    {
        face_line = "Face status: NO FACE";
        face_color = cv::Scalar(0, 0, 220);
    }
    else if (!cached.is_real)
    {
        face_line = "Face status: FAKE | live=" + fmtDouble(cached.live_score, 3);
        face_color = cv::Scalar(0, 0, 220);
    }
    else
    {
        face_line = "Face status: REAL | live=" + fmtDouble(cached.live_score, 3);
        face_color = cv::Scalar(0, 180, 0);
    }

    cv::putText(
        win,
        face_line,
        cv::Point(25, 163),
        cv::FONT_HERSHEY_SIMPLEX,
        0.60,
        face_color,
        2
    );

    cv::putText(
        win,
        "Recog score: " + fmtDouble(cached.match.score, 3),
        cv::Point(25, 195),
        cv::FONT_HERSHEY_SIMPLEX,
        0.55,
        cv::Scalar(220, 220, 220),
        1
    );

    cv::putText(
        win,
        add_state.message,
        cv::Point(25, 230),
        cv::FONT_HERSHEY_SIMPLEX,
        0.50,
        cv::Scalar(210, 210, 210),
        1
    );

    g_add_input_button_rect   = cv::Rect(25, 275, 140, 48);
    g_add_capture_button_rect = cv::Rect(180, 275, 140, 48);
    g_add_save_button_rect    = cv::Rect(335, 275, 120, 48);
    g_add_exit_button_rect    = cv::Rect(470, 275, 120, 48);

    cv::Scalar input_color = add_state.input_active
        ? cv::Scalar(100, 100, 0)
        : cv::Scalar(60, 60, 60);

    bool can_capture =
        !add_state.input_name.empty() &&
        add_state.captured_faces.size() < REGISTER_REQUIRED_IMAGES &&
        cached.has_face &&
        cached.has_aligned_face &&
        !cached.aligned_face.empty() &&
        cached.is_real;

    bool can_save =
        !add_state.input_name.empty() &&
        add_state.captured_faces.size() >= REGISTER_REQUIRED_IMAGES;

    cv::Scalar capture_color = can_capture
        ? cv::Scalar(0, 120, 0)
        : cv::Scalar(0, 0, 120);

    cv::Scalar save_color = can_save
        ? cv::Scalar(0, 115, 0)
        : cv::Scalar(70, 70, 70);

    drawButton(win, g_add_input_button_rect, "INPUT NAME", input_color);
    drawButton(win, g_add_capture_button_rect, "CAPTURE", capture_color);
    drawButton(win, g_add_save_button_rect, "SAVE", save_color);
    drawButton(win, g_add_exit_button_rect, "EXIT", cv::Scalar(80, 60, 60));

    cv::putText(
        win,
        "Keyboard: type when INPUT NAME is active | Backspace delete | Enter done",
        cv::Point(25, 360),
        cv::FONT_HERSHEY_SIMPLEX,
        0.43,
        cv::Scalar(190, 190, 190),
        1
    );

    cv::putText(
        win,
        "Save only works after 10 valid REAL face images.",
        cv::Point(25, 388),
        cv::FONT_HERSHEY_SIMPLEX,
        0.43,
        cv::Scalar(190, 190, 190),
        1
    );

    cv::imshow(ADD_USER_WINDOW_NAME, win);
}

// ====================== MODEL / USER DB ======================

void loadLiveModel(Live& live)
{
    ModelConfig config1 = {
        2.7f,
        0.0f,
        0.0f,
        80,
        80,
        "model_1",
        false
    };

    ModelConfig config2 = {
        4.0f,
        0.0f,
        0.0f,
        80,
        80,
        "model_2",
        false
    };

    vector<ModelConfig> configs;
    configs.emplace_back(config1);
    configs.emplace_back(config2);

    live.LoadModel(configs);
}

static cv::Mat prepareUserImageForFeature(
    const cv::Mat& input_img,
    const cv::Mat& gt_landmark_matrix
)
{
    if (input_img.empty())
    {
        return cv::Mat();
    }

    if (input_img.cols == 112 && input_img.rows == 112)
    {
        return input_img.clone();
    }

    std::vector<Bbox> faces = detect_mtcnn(input_img);

    if (!faces.empty())
    {
        Bbox largest = getLargestBboxFromBboxVec(faces);
        return alignFaceImage(input_img, largest, gt_landmark_matrix);
    }

    cv::Mat resized;
    cv::resize(
        input_img,
        resized,
        cv::Size(112, 112),
        0,
        0,
        INTER_LINEAR
    );

    return resized;
}

static void loadRegisteredUsers(
    Arcface& facereco,
    std::map<std::string, std::vector<cv::Mat>>& user_descriptors,
    const cv::Mat& gt_landmark_matrix
)
{
    ensureDir(USER_DIR);

    std::vector<cv::String> image_names;
    std::vector<cv::String> jpg_names;
    std::vector<cv::String> jpeg_names;
    std::vector<cv::String> png_names;

    cv::glob(USER_DIR + "/*.jpg", jpg_names, false);
    cv::glob(USER_DIR + "/*.jpeg", jpeg_names, false);
    cv::glob(USER_DIR + "/*.png", png_names, false);

    image_names.insert(image_names.end(), jpg_names.begin(), jpg_names.end());
    image_names.insert(image_names.end(), jpeg_names.begin(), jpeg_names.end());
    image_names.insert(image_names.end(), png_names.begin(), png_names.end());

    if (image_names.empty())
    {
        std::cerr << "[WARN] No image files found in: "
                  << USER_DIR << std::endl;
        std::cerr << "[WARN] You can register a new person from GUI using A / ADD USER."
                  << std::endl;
        return;
    }

    std::cout << "[INFO] Loading registered users from: "
              << USER_DIR << std::endl;
    std::cout << "[INFO] Total user images: "
              << image_names.size() << std::endl;

    int loaded = 0;
    int skipped = 0;

    for (const auto& img_name_cv : image_names)
    {
        std::string img_name = std::string(img_name_cv);
        std::string person_name = extractPersonName(img_name);

        cv::Mat img = cv::imread(img_name);

        if (img.empty())
        {
            std::cerr << "[WARN] Cannot read image: "
                      << img_name << std::endl;
            skipped++;
            continue;
        }

        cv::Mat face_img = prepareUserImageForFeature(
            img,
            gt_landmark_matrix
        );

        if (face_img.empty())
        {
            std::cerr << "[WARN] Cannot prepare face image: "
                      << img_name << std::endl;
            skipped++;
            continue;
        }

        cv::Mat recog_img = preprocessFaceForRecognition(face_img);

        cv::Mat descriptor = facereco.getFeature(recog_img);
        descriptor = Statistics::zScore(descriptor);

        user_descriptors[person_name].push_back(descriptor);
        loaded++;

        std::cout << "[LOAD] "
                  << person_name
                  << " <= "
                  << basenameFromPath(img_name)
                  << std::endl;
    }

    std::cout << "[INFO] Loaded descriptors: "
              << loaded << std::endl;
    std::cout << "[INFO] Skipped images: "
              << skipped << std::endl;
    std::cout << "[INFO] Registered people: "
              << user_descriptors.size() << std::endl;
}

static MatchResult findBestMatch(
    const std::map<std::string, std::vector<cv::Mat>>& user_descriptors,
    const cv::Mat& face_descriptor
)
{
    MatchResult result;
    result.name = "Unknown";
    result.score = -1.0;
    result.known = false;

    for (const auto& user_pair : user_descriptors)
    {
        const std::string& name = user_pair.first;
        const std::vector<cv::Mat>& descriptors = user_pair.second;

        for (const auto& ref_desc : descriptors)
        {
            double score = Statistics::cosineDistance(
                ref_desc,
                face_descriptor
            );

            if (score > result.score)
            {
                result.score = score;
                result.name = name;
            }
        }
    }

    if (result.score > face_thre)
    {
        result.known = true;
    }
    else
    {
        result.name = "Unknown";
        result.known = false;
    }

    return result;
}

static std::string makeRegisterFileName(
    const std::string& name,
    int image_index
)
{
    auto now = std::chrono::system_clock::now();

    long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()
        ).count();

    std::ostringstream oss;
    oss << USER_DIR << "/"
        << name << "_"
        << ms << "_"
        << std::setw(2) << std::setfill('0') << image_index
        << ".jpg";

    return oss.str();
}


static void openAddUserWindow(AddUserState& add_state)
{
    if (!add_state.window_open)
    {
        add_state.window_open = true;
        add_state.input_active = false;
        add_state.input_name.clear();
        add_state.safe_name.clear();
        add_state.captured_faces.clear();
        add_state.saved_current_batch = false;
        add_state.message = "Click INPUT NAME, type name, then capture 10 REAL faces.";

        cv::namedWindow(ADD_USER_WINDOW_NAME, cv::WINDOW_NORMAL);
        cv::resizeWindow(ADD_USER_WINDOW_NAME, 640, 420);
        cv::setMouseCallback(ADD_USER_WINDOW_NAME, onAddUserMouseEvent, nullptr);

        std::cout << "[ADD_USER] Open Add User window." << std::endl;
    }
}

static void closeAddUserWindow(AddUserState& add_state)
{
    if (!add_state.window_open)
    {
        return;
    }

    if (!add_state.saved_current_batch && !add_state.captured_faces.empty())
    {
        std::cout << "[ADD_USER] Exit without save. Discard "
                  << add_state.captured_faces.size()
                  << " captured images from RAM." << std::endl;
    }

    add_state.window_open = false;
    add_state.input_active = false;
    add_state.input_name.clear();
    add_state.safe_name.clear();
    add_state.captured_faces.clear();
    add_state.saved_current_batch = false;
    add_state.message = "Closed.";

    cv::destroyWindow(ADD_USER_WINDOW_NAME);
}

static void resetAddUserBatchAfterSave(AddUserState& add_state)
{
    add_state.input_active = false;
    add_state.input_name.clear();
    add_state.safe_name.clear();
    add_state.captured_faces.clear();
    add_state.saved_current_batch = false;
    add_state.message = "Saved successfully. Enter another name or click EXIT.";
}

static void handleAddUserTypingKey(AddUserState& add_state, int key)
{
    if (!add_state.window_open || !add_state.input_active)
    {
        return;
    }

    if (key == 13 || key == 10)
    {
        add_state.input_active = false;
        add_state.safe_name = makeSafeName(add_state.input_name);

        if (add_state.safe_name.empty())
        {
            add_state.message = "Invalid name. Use A-Z, a-z, 0-9, _ or -.";
        }
        else
        {
            add_state.input_name = add_state.safe_name;
            add_state.message = "Name OK. Capture 10 valid REAL face images.";
        }

        return;
    }

    if (key == 8 || key == 127)
    {
        if (!add_state.input_name.empty())
        {
            add_state.input_name.pop_back();
            add_state.safe_name = makeSafeName(add_state.input_name);
        }
        return;
    }

    if (key >= 32 && key <= 126)
    {
        char c = static_cast<char>(key);

        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')
        {
            if (add_state.input_name.size() < 32)
            {
                add_state.input_name.push_back(c);
                add_state.safe_name = makeSafeName(add_state.input_name);
            }
        }
        else
        {
            add_state.message = "Only A-Z, a-z, 0-9, _ and - are allowed.";
        }
    }
}

static bool captureAddUserImage(
    AddUserState& add_state,
    const CachedResult& cached
)
{
    if (!add_state.window_open)
    {
        return false;
    }

    add_state.safe_name = makeSafeName(add_state.input_name);

    if (add_state.safe_name.empty())
    {
        add_state.message = "Please click INPUT NAME and enter a valid name first.";
        std::cout << "[ADD_USER] Capture failed: empty/invalid name." << std::endl;
        return false;
    }

    if (add_state.captured_faces.size() >= REGISTER_REQUIRED_IMAGES)
    {
        add_state.message = "Already captured 10 images. Click SAVE.";
        return false;
    }

    if (!cached.has_face || !cached.has_aligned_face || cached.aligned_face.empty())
    {
        add_state.message = "Capture failed: no face detected.";
        std::cout << "[ADD_USER] Capture failed: no face detected." << std::endl;
        return false;
    }

    if (!cached.is_real)
    {
        add_state.message = "Capture failed: FAKE face. Need REAL face.";
        std::cout << "[ADD_USER] Capture failed: FAKE face. live_score="
                  << fmtDouble(cached.live_score, 3) << std::endl;
        return false;
    }

    add_state.captured_faces.push_back(cached.aligned_face.clone());
    add_state.saved_current_batch = false;

    std::cout << "[ADD_USER] Captured "
              << add_state.captured_faces.size() << "/"
              << REGISTER_REQUIRED_IMAGES
              << " | name=" << add_state.safe_name
              << " | live_score=" << fmtDouble(cached.live_score, 3)
              << " | recog_score=" << fmtDouble(cached.match.score, 3)
              << std::endl;

    if (add_state.captured_faces.size() >= REGISTER_REQUIRED_IMAGES)
    {
        add_state.message = "Captured 10/10. Click SAVE to write images.";
    }
    else
    {
        add_state.message = "Captured " +
            std::to_string((int)add_state.captured_faces.size()) + "/" +
            std::to_string(REGISTER_REQUIRED_IMAGES) +
            ". Continue capturing REAL faces.";
    }

    return true;
}

static bool saveAddUserBatch(
    AddUserState& add_state,
    Arcface& facereco,
    std::map<std::string, std::vector<cv::Mat>>& user_descriptors
)
{
    if (!add_state.window_open)
    {
        return false;
    }

    add_state.safe_name = makeSafeName(add_state.input_name);

    if (add_state.safe_name.empty())
    {
        add_state.message = "Save failed: invalid name.";
        return false;
    }

    if (add_state.captured_faces.size() < REGISTER_REQUIRED_IMAGES)
    {
        add_state.message = "Save disabled: need 10 valid images first.";
        std::cout << "[ADD_USER] Save blocked: only "
                  << add_state.captured_faces.size() << "/"
                  << REGISTER_REQUIRED_IMAGES << " images." << std::endl;
        return false;
    }

    ensureDir(USER_DIR);

    int saved_files = 0;

    for (int i = 0; i < (int)add_state.captured_faces.size(); i++)
    {
        const cv::Mat& face = add_state.captured_faces[i];

        if (face.empty())
        {
            continue;
        }

        std::string save_path = makeRegisterFileName(add_state.safe_name, i + 1);

        bool ok = cv::imwrite(save_path, face);

        if (!ok)
        {
            std::cerr << "[ADD_USER] Cannot save image: "
                      << save_path << std::endl;
            continue;
        }

        cv::Mat recog_img = preprocessFaceForRecognition(face);
        cv::Mat desc = facereco.getFeature(recog_img);
        desc = Statistics::zScore(desc);
        user_descriptors[add_state.safe_name].push_back(desc);

        saved_files++;

        std::cout << "[ADD_USER] Saved "
                  << saved_files << "/"
                  << REGISTER_REQUIRED_IMAGES
                  << ": " << save_path << std::endl;
    }

    if (saved_files >= REGISTER_REQUIRED_IMAGES)
    {
        std::cout << "[ADD_USER] Completed registration for: "
                  << add_state.safe_name << std::endl;

        add_state.saved_current_batch = true;
        add_state.message = "Saved " + add_state.safe_name +
                            " successfully. Window stays open.";
        resetAddUserBatchAfterSave(add_state);
        return true;
    }

    add_state.message = "Save failed: only " +
        std::to_string(saved_files) + "/" +
        std::to_string(REGISTER_REQUIRED_IMAGES) +
        " files saved. Check permission/disk.";

    return false;
}

// ====================== MAIN PIPELINE ======================

int MTCNNDetection()
{
    std::cout << "OpenCV Version: "
              << CV_MAJOR_VERSION << "."
              << CV_MINOR_VERSION << "."
              << CV_SUBMINOR_VERSION << std::endl;

    std::cout << "[INFO] project_path = "
              << project_path << std::endl;
    std::cout << "[INFO] USER_DIR     = "
              << USER_DIR << std::endl;
    std::cout << "[INFO] CSI CAMERA   = "
              << CAMERA_WIDTH << "x" << CAMERA_HEIGHT
              << "@" << CAMERA_FPS << std::endl;
    std::cout << "[INFO] PROCESS SIZE = "
              << PROCESS_WIDTH << "x" << PROCESS_HEIGHT << std::endl;
    std::cout << "[INFO] PROCESS STEP = every "
              << PROCESS_EVERY_N_FRAMES << " frames" << std::endl;
    std::cout << "[INFO] face_thre    = "
              << face_thre << std::endl;
    std::cout << "[INFO] true_thre    = "
              << true_thre << std::endl;

    std::cout << "[INFO] FACE PREPROCESS = "
              << (USE_FACE_PREPROCESSING ? "ON" : "OFF")
              << std::endl;
    std::cout << "[INFO] GAMMA CORRECTION = "
              << (USE_GAMMA_CORRECTION ? "ON" : "OFF")
              << " | gamma=" << GAMMA_VALUE
              << std::endl;
    std::cout << "[INFO] LAPLACE SHARPEN = "
              << (USE_LAPLACE_SHARPEN ? "ON" : "OFF")
              << " | alpha=" << LAPLACE_ALPHA
              << std::endl;

    ensureDir(USER_DIR);

    cv::Mat face_landmark_gt_matrix = createFaceLandmarkGTMatrix();

    Arcface facereco;

    std::map<std::string, std::vector<cv::Mat>> user_descriptors;

    loadRegisteredUsers(
        facereco,
        user_descriptors,
        face_landmark_gt_matrix
    );

    Live live;
    loadLiveModel(live);

    RpiCamRawCapture cap(CAMERA_WIDTH, CAMERA_HEIGHT, CAMERA_FPS);

    if (!cap.open())
    {
        std::cerr << "[ERROR] Cannot open CSI camera using rpicam-vid."
                  << std::endl;
        std::cerr << "[HINT] Test camera first:" << std::endl;
        std::cerr << "       rpicam-hello --list-cameras" << std::endl;
        std::cerr << "       rpicam-hello" << std::endl;
        std::cerr << "[HINT] Check log:" << std::endl;
        std::cerr << "       cat " << RPICAM_LOG << std::endl;
        return -1;
    }

    cv::namedWindow(MAIN_WINDOW_NAME, cv::WINDOW_NORMAL);
    cv::resizeWindow(
        MAIN_WINDOW_NAME,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT
    );
    cv::setMouseCallback(MAIN_WINDOW_NAME, onMainMouseEvent, nullptr);

    cv::Mat frame;
    cv::Mat process_frame;
    cv::Mat display_frame;

    CachedResult cached;
    AddUserState add_state;
    PerfStats perf;

    int frame_count = 0;

    // FPS thực:
    // - Không clamp về CAMERA_FPS nữa, vì clamp làm FPS đứng 30.0.
    // - Tính theo loop_ms sau khi đã camera + AI + draw + imshow + waitKey.
    // - Dùng EMA để số mượt nhưng vẫn thay đổi theo tải thật.
    double fps_real = 0.0;
    const double FPS_EMA_ALPHA = 0.18;

    std::cout << "[INFO] CSI camera started." << std::endl;
    std::cout << "[INFO] Main window: click ADD USER to open Add User window." << std::endl;
    std::cout << "[INFO] Add User window: INPUT NAME -> CAPTURE 10 REAL faces -> SAVE -> EXIT."
              << std::endl;
    std::cout << "[INFO] Press q on main window to quit." << std::endl;

    while (cap.isOpened())
    {
        auto loop_start = std::chrono::steady_clock::now();

        auto cam_start = std::chrono::steady_clock::now();
        frame = cap.getFrame();
        auto cam_end = std::chrono::steady_clock::now();
        perf.camera_ms = elapsedMs(cam_start, cam_end);

        if (frame.empty())
        {
            continue;
        }

        frame_count++;
        display_frame = frame.clone();

        bool should_process =
            (frame_count % PROCESS_EVERY_N_FRAMES == 0) ||
            (frame_count == 1) ||
            add_state.window_open;

        if (should_process)
        {
            PerfStats model_perf;
            model_perf.camera_ms = perf.camera_ms;

            auto infer_start = std::chrono::steady_clock::now();

            auto resize_start = std::chrono::steady_clock::now();
            cv::resize(
                frame,
                process_frame,
                cv::Size(PROCESS_WIDTH, PROCESS_HEIGHT),
                0,
                0,
                INTER_LINEAR
            );
            auto resize_end = std::chrono::steady_clock::now();
            model_perf.resize_ms = elapsedMs(resize_start, resize_end);

            float sx = static_cast<float>(frame.cols) / PROCESS_WIDTH;
            float sy = static_cast<float>(frame.rows) / PROCESS_HEIGHT;

            auto detect_start = std::chrono::steady_clock::now();
            std::vector<Bbox> faces_info = detect_mtcnn(process_frame);
            auto detect_end = std::chrono::steady_clock::now();
            model_perf.detect_ms = elapsedMs(detect_start, detect_end);

            if (!faces_info.empty())
            {
                Bbox largest_process_box =
                    getLargestBboxFromBboxVec(faces_info);

                Bbox largest_display_box =
                    scaleBbox(largest_process_box, sx, sy);

                LiveFaceBox live_face_box =
                    Bbox2LiveFaceBox(largest_process_box);

                auto align_pre_start = std::chrono::steady_clock::now();

                cv::Mat aligned_img = alignFaceImage(
                    process_frame,
                    largest_process_box,
                    face_landmark_gt_matrix
                );

                cv::Mat recog_img = preprocessFaceForRecognition(aligned_img);

                auto align_pre_end = std::chrono::steady_clock::now();
                model_perf.align_pre_ms = elapsedMs(
                    align_pre_start,
                    align_pre_end
                );

                auto recog_start = std::chrono::steady_clock::now();
                cv::Mat face_descriptor = facereco.getFeature(recog_img);
                auto recog_end = std::chrono::steady_clock::now();
                model_perf.recog_ms = elapsedMs(recog_start, recog_end);

                face_descriptor = Statistics::zScore(face_descriptor);

                auto match_start = std::chrono::steady_clock::now();
                MatchResult match = findBestMatch(
                    user_descriptors,
                    face_descriptor
                );
                auto match_end = std::chrono::steady_clock::now();
                model_perf.match_ms = elapsedMs(match_start, match_end);

                auto anti_start = std::chrono::steady_clock::now();
                double live_score = live.Detect(process_frame, live_face_box);
                auto anti_end = std::chrono::steady_clock::now();
                model_perf.anti_ms = elapsedMs(anti_start, anti_end);

                live_score = clamp01(live_score);
                bool is_real = live_score > true_thre;

                cached.has_face = true;
                cached.has_aligned_face = true;
                cached.match = match;
                cached.live_score = live_score;
                cached.is_real = is_real;
                cached.process_box = largest_process_box;
                cached.display_box = largest_display_box;
                cached.face_count = (int)faces_info.size();
                cached.aligned_face = aligned_img.clone();
                cached.result_frame_id = frame_count;

                if (DRAW_ALL_FACES)
                {
                    drawAllDetectedFaces(display_frame, faces_info);
                }
            }
            else
            {
                cached.has_face = false;
                cached.has_aligned_face = false;
                cached.match = {"Unknown", -1.0, false};
                cached.live_score = 0.0;
                cached.is_real = false;
                cached.face_count = 0;
                cached.aligned_face.release();
                cached.result_frame_id = frame_count;
            }

            auto infer_end = std::chrono::steady_clock::now();
            model_perf.infer_ms = elapsedMs(infer_start, infer_end);

            cached.perf = model_perf;
            perf = model_perf;
        }

        if (cached.has_face)
        {
            drawMainFaceResult(display_frame, cached);
        }
        else
        {
            drawNoFaceUI(display_frame);
        }

        drawMainBottomBar(display_frame);

        cv::imshow(MAIN_WINDOW_NAME, display_frame);

        if (add_state.window_open)
        {
            drawAddUserWindow(add_state, cached);
        }

        int key = cv::waitKey(1);

        auto loop_end = std::chrono::steady_clock::now();
        perf.loop_ms = elapsedMs(loop_start, loop_end);
        cached.perf.loop_ms = perf.loop_ms;

        double fps_inst = 0.0;

        if (perf.loop_ms > 0.0001)
        {
            fps_inst = 1000.0 / perf.loop_ms;
        }

        if (fps_inst > 0.0)
        {
            if (fps_real <= 0.0)
            {
                fps_real = fps_inst;
            }
            else
            {
                fps_real =
                    (1.0 - FPS_EMA_ALPHA) * fps_real +
                    FPS_EMA_ALPHA * fps_inst;
            }
        }

        perf.fps_real = fps_real;
        cached.perf.fps_real = fps_real;

        bool request_main_add = g_main_add_clicked;
        bool request_add_input = g_add_input_clicked;
        bool request_add_capture = g_add_capture_clicked;
        bool request_add_save = g_add_save_clicked;
        bool request_add_exit = g_add_exit_clicked;

        g_main_add_clicked = false;
        g_add_input_clicked = false;
        g_add_capture_clicked = false;
        g_add_save_clicked = false;
        g_add_exit_clicked = false;

        if (request_main_add)
        {
            openAddUserWindow(add_state);
        }

        if (add_state.window_open)
        {
            if (request_add_input)
            {
                add_state.input_active = true;
                add_state.saved_current_batch = false;
                add_state.message = "Typing name. Press Enter when done.";
            }

            if (request_add_capture)
            {
                captureAddUserImage(add_state, cached);
            }

            if (request_add_save)
            {
                saveAddUserBatch(
                    add_state,
                    facereco,
                    user_descriptors
                );
            }

            if (request_add_exit)
            {
                closeAddUserWindow(add_state);
            }
        }

        if (key >= 0)
        {
            if (add_state.window_open && add_state.input_active)
            {
                handleAddUserTypingKey(add_state, key);
            }
            else
            {
                if (key == 'q' || key == 'Q')
                {
                    if (!add_state.window_open)
                    {
                        break;
                    }
                }
                else if (key == 'a' || key == 'A')
                {
                    openAddUserWindow(add_state);
                }
                else if (add_state.window_open && (key == 'c' || key == 'C'))
                {
                    captureAddUserImage(add_state, cached);
                }
                else if (add_state.window_open && (key == 's' || key == 'S'))
                {
                    saveAddUserBatch(
                        add_state,
                        facereco,
                        user_descriptors
                    );
                }
                else if (add_state.window_open && key == 27)
                {
                    closeAddUserWindow(add_state);
                }
            }
        }

        if (frame_count % LOG_EVERY_N_FRAMES == 0)
        {
            std::cout << "[FRAME " << frame_count << "] ";

            if (cached.has_face)
            {
                std::cout << "name=" << cached.match.name
                          << " | face=" << (cached.is_real ? "REAL" : "FAKE")
                          << " | live_score=" << fmtDouble(cached.live_score, 3)
                          << " | recog_score=" << fmtDouble(cached.match.score, 3)
                          << " | fps_real=" << fmtDouble(perf.fps_real, 1)
                          << " | fps_inst=" << fmtDouble(fps_inst, 1)
                          << " | loop_ms=" << fmtDouble(perf.loop_ms, 1)
                          << " | infer_ms=" << fmtDouble(cached.perf.infer_ms, 1)
                          << " | detect_ms=" << fmtDouble(cached.perf.detect_ms, 1)
                          << " | recog_ms=" << fmtDouble(cached.perf.recog_ms, 1)
                          << " | anti_ms=" << fmtDouble(cached.perf.anti_ms, 1)
                          << " | align_pre_ms=" << fmtDouble(cached.perf.align_pre_ms, 1)
                          << " | match_ms=" << fmtDouble(cached.perf.match_ms, 1)
                          << " | camera_ms=" << fmtDouble(perf.camera_ms, 1)
                          << std::endl;
            }
            else
            {
                std::cout << "name=Unknown"
                          << " | face=NO_FACE"
                          << " | live_score=0.000"
                          << " | recog_score=-1.000"
                          << " | fps_real=" << fmtDouble(perf.fps_real, 1)
                          << " | fps_inst=" << fmtDouble(fps_inst, 1)
                          << " | loop_ms=" << fmtDouble(perf.loop_ms, 1)
                          << " | infer_ms=" << fmtDouble(cached.perf.infer_ms, 1)
                          << " | detect_ms=" << fmtDouble(cached.perf.detect_ms, 1)
                          << " | recog_ms=0.0"
                          << " | anti_ms=0.0"
                          << " | align_pre_ms=0.0"
                          << " | match_ms=0.0"
                          << " | camera_ms=" << fmtDouble(perf.camera_ms, 1)
                          << std::endl;
            }
        }
    }

    if (add_state.window_open)
    {
        closeAddUserWindow(add_state);
    }

    cap.close();
    cv::destroyAllWindows();

    std::cout << "[INFO] CSI camera stopped." << std::endl;

    return 0;
}