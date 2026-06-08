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
// - Unlock: REAL + Known 2s -> send '1'; wait 5s -> count 2s again to resend '1'
// - ESP32 feedback on same UART: receive '2'=WRONG_PIN, '3'=FACE_ACCEPTED, '4'=KEYPAD_ACCEPTED
// - Pi writes Firestore logs and local snapshots for face fail + ESP32 feedback events
// - Press q to quit main window
//

#include <math.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cerrno>
#include <ctime>
#include <deque>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

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

// Folder lưu ảnh minh chứng mỗi lần Pi tạo sự kiện log Firestore.
// Thư mục này nằm ngang hàng với users/:
//   <project_path>/users
//   <project_path>/access_log_images
static const std::string ACCESS_LOG_IMAGE_DIR =
    project_path + "/access_log_images";

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


// ====================== FIRESTORE CONFIG ======================
//
// Firebase Authentication account dùng chung cho Raspberry Pi và ESP32.
// Raspberry Pi tạo log Firestore cho:
//   - SPOOF_DETECTED : phát hiện FAKE FACE
//   - UNKNOWN_FACE   : REAL FACE nhưng không nhận diện được
//   - WRONG_PIN      : ESP32 gửi phản hồi '2'
//   - ACCESS_ACCEPTED bằng face  : ESP32 gửi phản hồi '3'
//   - ACCESS_ACCEPTED bằng keypad: ESP32 gửi phản hồi '4'
//
static const bool ENABLE_FIRESTORE_LOG = true;

static const std::string FIREBASE_PROJECT_ID = "smartlock-7f70d";
static const std::string FIREBASE_WEB_API_KEY =
    "AIzaSyBNz7bVsc8PsVJRO8IXfWZvw7YlRQ9LJa0";
static const std::string FIREBASE_DEVICE_EMAIL =
    "smartlock-device@gmail.com";
static const std::string FIREBASE_DEVICE_PASSWORD =
    "123456";

// Chỉ ghi Firestore khi MỘT ĐIỀU KIỆN FAIL giữ liên tục đủ 5 giây.
//
// Hai điều kiện được tính timer ĐỘC LẬP:
//   - !is_real         giữ đủ 5 giây -> SPOOF_DETECTED
//   - !match.known     giữ đủ 5 giây -> UNKNOWN_FACE
//
// Ví dụ:
//   - Name luôn Unknown trong 5 giây nhưng REAL/FAKE nhảy:
//       timer UNKNOWN vẫn chạy và ghi UNKNOWN_FACE.
//   - Face luôn FAKE trong 5 giây nhưng match Known/Unknown nhảy:
//       timer FAKE vẫn chạy và ghi SPOOF_DETECTED.
//   - Cả hai cùng đủ 5 giây ở cùng một lần kiểm tra:
//       ưu tiên ghi SPOOF_DETECTED.
//
static const double FIRESTORE_DENIED_HOLD_SECONDS = 5.0;

// Nếu một phiên truy cập vẫn luôn FAIL liên tục đủ 30 giây mà không reset,
// mở chu kỳ cảnh báo mới. Ví dụ: log ở giây 5, cho phép log tiếp ở giây 35,
// rồi giây 65 nếu người/ảnh giả vẫn đứng trước camera và tiếp tục fail.
static const double FIRESTORE_CONTINUOUS_FAIL_CYCLE_SECONDS = 30.0;

// Nếu mạng lỗi lâu, chỉ giữ tối đa 32 log đang chờ trong RAM.
static const std::size_t FIRESTORE_MAX_QUEUE_SIZE = 32;

// Ảnh log được lưu cục bộ trong ACCESS_LOG_IMAGE_DIR.
// Trường "image" gửi lên Firestore vẫn giữ "NOT_SAVED_IN_DEMO"
// để khớp Rules hiện tại; ảnh cục bộ không phải Firebase Storage.
static const bool SAVE_LOCAL_FIRESTORE_SNAPSHOT = true;

// ====================== ESP32 UART UNLOCK CONFIG ======================
//
// Giữ đúng cơ chế đầu ra của code FaceNet:
//   /dev/serial0, 9600 baud, gửi ASCII '1' để mở và '0' để đóng.
//
// Cơ chế unlock:
//   1. REAL FACE + Known liên tục đủ 2 giây -> gửi ASCII '1'.
//   2. Sau khi đã gửi '1', chờ 5 giây.
//   3. Nếu khuôn mặt vẫn hợp lệ, bắt đầu đếm lại 2 giây và gửi '1' lần tiếp.
//   4. Nếu mất hợp lệ ở bất kỳ thời điểm nào -> gửi ASCII '0' và reset.
//
// Lưu ý: lần gửi '1' tiếp theo dùng force=true vì đây là tín hiệu trigger
// mở khóa mới, kể cả trạng thái UART trước đó vẫn đang là OPEN.
//
static const bool ENABLE_ESP32_UNLOCK_SIGNAL = true;
static const std::string ESP32_SERIAL_PORT = "/dev/serial0";
static const int ESP32_SERIAL_BAUD = 9600;
static const double VALID_FACE_UNLOCK_HOLD_SECONDS = 2.0;
static const double UNLOCK_RECHECK_DELAY_SECONDS = 5.0;

// ESP32 gửi ngược về Pi qua cùng UART:
//   '2' -> WRONG_PIN, method="keypad", result="DENIED"
//   '3' -> FACE_ACCESS_ACCEPTED, method="face", result="GRANTED"
//   '4' -> KEYPAD_ACCESS_ACCEPTED, method="keypad", result="GRANTED"
//
// Với lệnh '3', Pi ưu tiên lấy userName từ lần face unlock gần nhất.
// Nếu quá cửa sổ này thì fallback sang cached result hiện tại, sau đó mới dùng "Unknown".
static const double FACE_GRANTED_CONTEXT_WINDOW_SECONDS = 6.0;
static const double ESP32_FEEDBACK_LOG_DEBOUNCE_SECONDS = 1.0;
static const int ESP32_UART_MAX_READ_PER_LOOP = 16;

// ====================================================

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


/*
 * Forward declaration:
 * PiDeniedEventGate nằm phía trên phần UTILS nhưng cần in số thời gian bằng fmtDouble().
 * Hàm thật được định nghĩa phía dưới trong phần UTILS.
 */
static std::string fmtDouble(double v, int precision);



// ====================== FIRESTORE ASYNC LOGGER ======================
//
// Gửi HTTPS trên thread riêng để không chặn camera + inference loop.
// createdAt dùng REQUEST_TIME của Firestore server để khớp Rule:
//     request.resource.data.createdAt == request.time
//
struct FirestoreLogEvent
{
    std::string document_id;
    std::string user_name = "Unknown";
    std::string method = "face";
    std::string result = "DENIED";
    std::string reason;
    std::string image_field = "NOT_SAVED_IN_DEMO";
    std::string captured_at_local;
    double live_score = 0.0;
    double recognition_score = -1.0;
    cv::Mat snapshot_frame;
};

class FirestoreLogger
{
public:
    FirestoreLogger()
        : enabled_(false),
          running_(false),
          curl_initialized_(false),
          token_expire_time_(std::chrono::system_clock::time_point::min()),
          document_counter_(0)
    {
    }

    ~FirestoreLogger()
    {
        stop();
    }

    bool start()
    {
        if (enabled_.load())
        {
            return true;
        }

        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        {
            std::cerr << "[FIRESTORE] curl_global_init failed. Logger OFF."
                      << std::endl;
            return false;
        }

        curl_initialized_ = true;
        enabled_.store(true);
        running_.store(true);
        worker_thread_ = std::thread(&FirestoreLogger::workerLoop, this);

        std::cout << "[FIRESTORE] Logger ON."
                  << " Pi can send face denied, face granted, keypad granted, and wrong PIN logs."
                  << std::endl;
        return true;
    }

    void stop()
    {
        if (!enabled_.load())
        {
            return;
        }

        running_.store(false);
        queue_cv_.notify_all();

        if (worker_thread_.joinable())
        {
            worker_thread_.join();
        }

        enabled_.store(false);

        if (curl_initialized_)
        {
            curl_global_cleanup();
            curl_initialized_ = false;
        }

        std::cout << "[FIRESTORE] Logger stopped." << std::endl;
    }

    bool isEnabled() const
    {
        return enabled_.load();
    }

    static bool isAllowedAccessLogFields(
        const std::string& method,
        const std::string& result,
        const std::string& reason,
        const std::string& user_name
    )
    {
        const bool method_ok = (method == "face" || method == "keypad");
        const bool result_ok = (result == "GRANTED" || result == "DENIED");
        const bool reason_ok =
            reason == "ACCESS_ACCEPTED" ||
            reason == "UNKNOWN_FACE" ||
            reason == "SPOOF_DETECTED" ||
            reason == "WRONG_PIN";

        if (!method_ok || !result_ok || !reason_ok || user_name.empty())
        {
            return false;
        }

        // Khớp Firestore Rules:
        // - Face denied: chỉ UNKNOWN_FACE / SPOOF_DETECTED và userName="Unknown".
        if (method == "face" &&
            result == "DENIED" &&
            user_name == "Unknown" &&
            (reason == "UNKNOWN_FACE" || reason == "SPOOF_DETECTED"))
        {
            return true;
        }

        // - Keypad denied: chỉ WRONG_PIN và userName="Unknown".
        if (method == "keypad" &&
            result == "DENIED" &&
            user_name == "Unknown" &&
            reason == "WRONG_PIN")
        {
            return true;
        }

        // - Granted: method face/keypad đều được, reason phải ACCESS_ACCEPTED.
        if ((method == "face" || method == "keypad") &&
            result == "GRANTED" &&
            reason == "ACCESS_ACCEPTED")
        {
            return true;
        }

        return false;
    }

    bool enqueueAccessEvent(
        const std::string& method,
        const std::string& result,
        const std::string& reason,
        const std::string& user_name,
        const cv::Mat& snapshot_frame,
        const CachedResult& cached
    )
    {
        if (!enabled_.load())
        {
            return false;
        }

        if (!isAllowedAccessLogFields(method, result, reason, user_name))
        {
            std::cerr << "[FIRESTORE] Block invalid access event: "
                      << "method=" << method
                      << " | result=" << result
                      << " | reason=" << reason
                      << " | userName=" << user_name
                      << std::endl;
            return false;
        }

        FirestoreLogEvent event;
        event.document_id = createDocumentId();
        event.user_name = user_name;
        event.method = method;
        event.result = result;
        event.reason = reason;
        event.captured_at_local = createLocalTimestamp();
        event.live_score = cached.live_score;
        event.recognition_score = cached.match.score;

        if (!snapshot_frame.empty())
        {
            event.snapshot_frame = snapshot_frame.clone();
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            if (event_queue_.size() >= FIRESTORE_MAX_QUEUE_SIZE)
            {
                event_queue_.pop_front();
                std::cerr << "[FIRESTORE] Queue full, drop oldest log."
                          << std::endl;
            }

            event_queue_.push_back(std::move(event));
        }

        queue_cv_.notify_one();
        return true;
    }

    bool enqueueDeniedEvent(
        const std::string& reason,
        const cv::Mat& snapshot_frame,
        const CachedResult& cached
    )
    {
        // Giữ API cũ cho PiDeniedEventGate: face denied luôn log Unknown.
        return enqueueAccessEvent(
            "face",
            "DENIED",
            reason,
            "Unknown",
            snapshot_frame,
            cached
        );
    }

private:
    struct HttpResponse
    {
        long status_code = 0;
        std::string body;
        std::string error;
    };

    static size_t curlWriteCallback(
        void* contents,
        size_t size,
        size_t nmemb,
        void* user_data
    )
    {
        const size_t total = size * nmemb;
        std::string* output = static_cast<std::string*>(user_data);
        output->append(static_cast<char*>(contents), total);
        return total;
    }

    static bool isSuccess(long status_code)
    {
        return status_code >= 200 && status_code < 300;
    }

    static std::string shortResponse(const std::string& body)
    {
        const size_t max_size = 250;

        if (body.size() <= max_size)
        {
            return body;
        }

        return body.substr(0, max_size) + "...";
    }

    HttpResponse post(
        const std::string& url,
        const std::string& body,
        const std::string& content_type,
        const std::string& bearer_token = ""
    )
    {
        HttpResponse response;
        CURL* curl = curl_easy_init();

        if (!curl)
        {
            response.error = "curl_easy_init failed";
            return response;
        }

        struct curl_slist* headers = nullptr;
        std::string type_header = "Content-Type: " + content_type;
        headers = curl_slist_append(headers, type_header.c_str());

        std::string auth_header;

        if (!bearer_token.empty())
        {
            auth_header = "Authorization: Bearer " + bearer_token;
            headers = curl_slist_append(headers, auth_header.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "SmartLockPi-Firestore/1.0");

        CURLcode curl_result = curl_easy_perform(curl);

        if (curl_result != CURLE_OK)
        {
            response.error = curl_easy_strerror(curl_result);
        }

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        return response;
    }

    std::string urlEncode(const std::string& input)
    {
        CURL* curl = curl_easy_init();

        if (!curl)
        {
            return input;
        }

        char* escaped = curl_easy_escape(
            curl,
            input.c_str(),
            static_cast<int>(input.size())
        );

        std::string output = escaped ? std::string(escaped) : input;

        if (escaped)
        {
            curl_free(escaped);
        }

        curl_easy_cleanup(curl);
        return output;
    }

    bool firebaseSignIn()
    {
        nlohmann::json auth_body;
        auth_body["email"] = FIREBASE_DEVICE_EMAIL;
        auth_body["password"] = FIREBASE_DEVICE_PASSWORD;
        auth_body["returnSecureToken"] = true;

        std::string url =
            "https://identitytoolkit.googleapis.com/v1/accounts:"
            "signInWithPassword?key=" + FIREBASE_WEB_API_KEY;

        HttpResponse response = post(
            url,
            auth_body.dump(),
            "application/json"
        );

        if (!isSuccess(response.status_code))
        {
            std::cerr << "[FIRESTORE] Auth failed HTTP "
                      << response.status_code << ": "
                      << shortResponse(response.body) << std::endl;
            return false;
        }

        nlohmann::json result =
            nlohmann::json::parse(response.body, nullptr, false);

        if (result.is_discarded())
        {
            std::cerr << "[FIRESTORE] Cannot parse Auth JSON." << std::endl;
            return false;
        }

        id_token_ = result.value("idToken", "");
        refresh_token_ = result.value("refreshToken", "");

        long expires_seconds = 3600;

        try
        {
            expires_seconds = std::stol(result.value("expiresIn", "3600"));
        }
        catch (...)
        {
            expires_seconds = 3600;
        }

        token_expire_time_ =
            std::chrono::system_clock::now() +
            std::chrono::seconds(expires_seconds);

        if (id_token_.empty() || refresh_token_.empty())
        {
            std::cerr << "[FIRESTORE] Auth token empty." << std::endl;
            return false;
        }

        std::cout << "[FIRESTORE] Firebase Auth OK." << std::endl;
        return true;
    }

    bool refreshIdToken()
    {
        if (refresh_token_.empty())
        {
            return firebaseSignIn();
        }

        std::string url =
            "https://securetoken.googleapis.com/v1/token?key=" +
            FIREBASE_WEB_API_KEY;

        std::string body =
            "grant_type=refresh_token&refresh_token=" +
            urlEncode(refresh_token_);

        HttpResponse response = post(
            url,
            body,
            "application/x-www-form-urlencoded"
        );

        if (!isSuccess(response.status_code))
        {
            std::cerr << "[FIRESTORE] Refresh token failed, sign in again."
                      << std::endl;
            return firebaseSignIn();
        }

        nlohmann::json result =
            nlohmann::json::parse(response.body, nullptr, false);

        if (result.is_discarded())
        {
            return firebaseSignIn();
        }

        id_token_ = result.value("id_token", "");
        refresh_token_ = result.value("refresh_token", refresh_token_);

        long expires_seconds = 3600;

        try
        {
            expires_seconds = std::stol(result.value("expires_in", "3600"));
        }
        catch (...)
        {
            expires_seconds = 3600;
        }

        token_expire_time_ =
            std::chrono::system_clock::now() +
            std::chrono::seconds(expires_seconds);

        return !id_token_.empty();
    }

    bool ensureValidIdToken()
    {
        if (id_token_.empty())
        {
            return firebaseSignIn();
        }

        auto now = std::chrono::system_clock::now();

        if (now + std::chrono::seconds(90) >= token_expire_time_)
        {
            return refreshIdToken();
        }

        return true;
    }

    std::string createDocumentId()
    {
        auto now = std::chrono::system_clock::now();

        long long millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();

        std::ostringstream output;
        output << "log_door_01_"
               << millis << "_"
               << document_counter_.fetch_add(1);

        return output.str();
    }

    static std::string createLocalTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        std::time_t now_time = std::chrono::system_clock::to_time_t(now);

        std::tm time_info;
        localtime_r(&now_time, &time_info);

        char output[32];
        std::strftime(
            output,
            sizeof(output),
            "%Y-%m-%d %H:%M:%S",
            &time_info
        );

        return std::string(output);
    }

    bool saveAnnotatedSnapshot(const FirestoreLogEvent& event)
    {
        if (!SAVE_LOCAL_FIRESTORE_SNAPSHOT)
        {
            return true;
        }

        if (event.snapshot_frame.empty())
        {
            std::cerr << "[SNAPSHOT] Skip empty snapshot for "
                      << event.document_id << std::endl;
            return false;
        }

        if (::mkdir(ACCESS_LOG_IMAGE_DIR.c_str(), 0755) != 0 &&
            errno != EEXIST)
        {
            std::cerr << "[SNAPSHOT] Cannot create folder: "
                      << ACCESS_LOG_IMAGE_DIR << std::endl;
            return false;
        }

        cv::Mat saved_frame = event.snapshot_frame.clone();

        int panel_x = 10;
        int panel_w = std::min(saved_frame.cols - 20, 610);
        int panel_h = 206;
        int panel_y = std::max(10, saved_frame.rows - panel_h - 10);

        if (panel_w > 0)
        {
            cv::rectangle(
                saved_frame,
                cv::Rect(panel_x, panel_y, panel_w, panel_h),
                cv::Scalar(0, 0, 0),
                cv::FILLED
            );

            std::vector<std::string> lines = {
                "FIRESTORE ACCESS LOG SNAPSHOT",
                "document: " + event.document_id,
                "userName: " + event.user_name + " | method: " + event.method,
                "result: " + event.result + " | reason: " + event.reason,
                "createdAt: SERVER_TIMESTAMP",
                "capturedAtLocal: " + event.captured_at_local,
                "image: " + event.image_field,
                "live_score: " + fmtDouble(event.live_score, 3) +
                    " | recog_score: " + fmtDouble(event.recognition_score, 3)
            };

            int line_y = panel_y + 25;
            for (std::size_t i = 0; i < lines.size(); ++i)
            {
                cv::Scalar color =
                    (i == 0) ? cv::Scalar(0, 255, 255) :
                    ((event.reason == "SPOOF_DETECTED" && i == 3)
                        ? cv::Scalar(0, 0, 255)
                        : cv::Scalar(255, 255, 255));

                cv::putText(
                    saved_frame,
                    lines[i],
                    cv::Point(panel_x + 10, line_y),
                    cv::FONT_HERSHEY_SIMPLEX,
                    0.46,
                    color,
                    1
                );
                line_y += 23;
            }
        }

        const std::string save_path =
            ACCESS_LOG_IMAGE_DIR + "/" +
            event.document_id + "_" + event.reason + ".jpg";

        if (!cv::imwrite(save_path, saved_frame))
        {
            std::cerr << "[SNAPSHOT] Cannot save image: "
                      << save_path << std::endl;
            return false;
        }

        std::cout << "[SNAPSHOT] SAVED " << save_path << std::endl;
        return true;
    }

    bool sendToFirestore(const FirestoreLogEvent& event)
    {
        // Chặn cứng thêm lần nữa trước khi thực sự gửi HTTPS,
        // khớp với Firestore Rules hiện tại.
        if (!isAllowedAccessLogFields(
                event.method,
                event.result,
                event.reason,
                event.user_name))
        {
            std::cerr << "[FIRESTORE] Refuse to send invalid event: "
                      << "method=" << event.method
                      << " | result=" << event.result
                      << " | reason=" << event.reason
                      << " | userName=" << event.user_name
                      << std::endl;
            return false;
        }

        if (!ensureValidIdToken())
        {
            return false;
        }

        std::string document_name =
            "projects/" + FIREBASE_PROJECT_ID +
            "/databases/(default)/documents/access_history/" +
            event.document_id;

        nlohmann::json fields;
        fields["userName"]["stringValue"] = event.user_name;
        fields["method"]["stringValue"] = event.method;
        fields["result"]["stringValue"] = event.result;
        fields["reason"]["stringValue"] = event.reason;
        fields["image"]["stringValue"] = event.image_field;

        nlohmann::json transform;
        transform["fieldPath"] = "createdAt";
        transform["setToServerValue"] = "REQUEST_TIME";

        nlohmann::json write;
        write["update"]["name"] = document_name;
        write["update"]["fields"] = fields;
        write["updateTransforms"] = nlohmann::json::array({transform});
        write["currentDocument"]["exists"] = false;

        nlohmann::json commit_request;
        commit_request["writes"] = nlohmann::json::array({write});

        std::string url =
            "https://firestore.googleapis.com/v1/projects/" +
            FIREBASE_PROJECT_ID +
            "/databases/(default)/documents:commit";

        HttpResponse response = post(
            url,
            commit_request.dump(),
            "application/json",
            id_token_
        );

        if (response.status_code == 401 && firebaseSignIn())
        {
            response = post(
                url,
                commit_request.dump(),
                "application/json",
                id_token_
            );
        }

        if (!isSuccess(response.status_code))
        {
            std::cerr << "[FIRESTORE] Write failed HTTP "
                      << response.status_code << ": "
                      << shortResponse(response.body) << std::endl;
            return false;
        }

        std::cout << "[FIRESTORE] CREATED access_history/"
                  << event.document_id
                  << " | reason=" << event.reason << std::endl;
        return true;
    }

    void workerLoop()
    {
        while (true)
        {
            FirestoreLogEvent event;

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);

                queue_cv_.wait(
                    lock,
                    [this]()
                    {
                        return !running_.load() || !event_queue_.empty();
                    }
                );

                if (!running_.load() && event_queue_.empty())
                {
                    break;
                }

                if (event_queue_.empty())
                {
                    continue;
                }

                event = event_queue_.front();
                event_queue_.pop_front();
            }

            // Lưu ảnh cục bộ ở background thread ngay khi event Firestore được tạo.
            // Nếu mạng lỗi, ảnh bằng chứng vẫn còn trên Raspberry Pi.
            saveAnnotatedSnapshot(event);

            bool sent = false;

            for (int attempt = 1; attempt <= 3 && !sent; attempt++)
            {
                sent = sendToFirestore(event);

                if (!sent && attempt < 3)
                {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(attempt * 500)
                    );
                }
            }

            if (!sent)
            {
                std::cerr << "[FIRESTORE] Drop log after retry: "
                          << event.reason << std::endl;
            }
        }
    }

private:
    std::atomic<bool> enabled_;
    std::atomic<bool> running_;
    bool curl_initialized_;

    std::thread worker_thread_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<FirestoreLogEvent> event_queue_;

    std::string id_token_;
    std::string refresh_token_;
    std::chrono::system_clock::time_point token_expire_time_;
    std::atomic<std::uint64_t> document_counter_;
};

// ====================== PI FIRESTORE EVENT GATE ======================
//
// Chỉ gửi một log cho một lần đối tượng đứng trước camera.
// Reset sự kiện sau khi:
//   - không còn mặt trong khung hình; hoặc
//   - nhận diện được người hợp lệ.
//
// Nếu kết quả liveness/recognition dao động trong lúc cùng một đối tượng vẫn
// đứng trước camera, code không tạo thêm log thứ hai sau khi đã gửi.
//
class PiDeniedEventGate
{
public:
    PiDeniedEventGate(
        double required_hold_seconds,
        double continuous_fail_cycle_seconds
    )
        : required_hold_seconds_(std::max(0.1, required_hold_seconds)),
          continuous_fail_cycle_seconds_(
              std::max(required_hold_seconds_, continuous_fail_cycle_seconds)
          ),
          cycle_started_(false),
          fake_timer_started_(false),
          unknown_timer_started_(false),
          log_already_sent_in_cycle_(false)
    {
    }

    void reset()
    {
        cycle_started_ = false;
        fake_timer_started_ = false;
        unknown_timer_started_ = false;
        log_already_sent_in_cycle_ = false;
    }

    void observe(
        const CachedResult& cached,
        const cv::Mat& snapshot_frame,
        bool suppress_logging,
        FirestoreLogger& logger
    )
    {
        /*
         * Một "phiên fail liên tục" chỉ tồn tại khi vẫn thấy mặt và cửa
         * chưa được chấp nhận mở:
         *   - UNKNOWN_FACE: !cached.match.known
         *   - SPOOF_DETECTED: !cached.is_real
         *
         * Reset toàn bộ khi:
         *   - đang ADD USER;
         *   - không còn mặt;
         *   - REAL + Known, tức truy cập khuôn mặt hợp lệ.
         */
        const bool access_is_valid =
            cached.has_face &&
            cached.is_real &&
            cached.match.known;

        const bool denied_is_active =
            cached.has_face &&
            (!cached.is_real || !cached.match.known);

        if (suppress_logging || !denied_is_active || access_is_valid)
        {
            reset();
            return;
        }

        const auto now = std::chrono::steady_clock::now();

        /*
         * Chu kỳ fail chung:
         * - Bắt đầu ngay từ inference fail đầu tiên.
         * - UNKNOWN/FAKE thay đổi qua lại KHÔNG reset chu kỳ 30 giây,
         *   miễn là vẫn đang fail.
         * - Đủ 30 giây fail liên tục thì mở một chu kỳ log mới.
         */
        if (!cycle_started_)
        {
            startNewCycle(now, false);
        }
        else
        {
            const double cycle_seconds =
                std::chrono::duration<double>(now - cycle_start_time_).count();

            if (cycle_seconds >= continuous_fail_cycle_seconds_)
            {
                startNewCycle(now, true);
            }
        }

        /*
         * Nếu chu kỳ hiện tại đã ghi một log, vẫn theo dõi phiên fail 30 giây
         * ở phần trên, nhưng chưa xét log mới cho đến khi sang chu kỳ tiếp.
         */
        if (log_already_sent_in_cycle_)
        {
            return;
        }

        /*
         * Hai timer reason chạy độc lập trong MỖI chu kỳ 30 giây:
         * - REAL/FAKE nhảy không reset UNKNOWN timer.
         * - Known/Unknown nhảy không reset FAKE timer.
         */
        const bool fake_active = !cached.is_real;
        const bool unknown_active = !cached.match.known;

        updateConditionTimer(
            fake_active,
            fake_timer_started_,
            fake_start_time_,
            "SPOOF_DETECTED",
            now
        );

        updateConditionTimer(
            unknown_active,
            unknown_timer_started_,
            unknown_start_time_,
            "UNKNOWN_FACE",
            now
        );

        const double fake_seconds = elapsedTimerSeconds(
            fake_timer_started_,
            fake_start_time_,
            now
        );

        const double unknown_seconds = elapsedTimerSeconds(
            unknown_timer_started_,
            unknown_start_time_,
            now
        );

        std::string reason_to_log;
        double held_seconds = 0.0;

        /*
         * Nếu cả hai fail cùng đạt 5 giây tại một inference, ưu tiên
         * SPOOF_DETECTED vì đây là hành vi giả mạo.
         */
        if (fake_active &&
            fake_timer_started_ &&
            fake_seconds >= required_hold_seconds_)
        {
            reason_to_log = "SPOOF_DETECTED";
            held_seconds = fake_seconds;
        }
        else if (unknown_active &&
                 unknown_timer_started_ &&
                 unknown_seconds >= required_hold_seconds_)
        {
            reason_to_log = "UNKNOWN_FACE";
            held_seconds = unknown_seconds;
        }

        if (reason_to_log.empty())
        {
            return;
        }

        if (logger.enqueueDeniedEvent(reason_to_log, snapshot_frame, cached))
        {
            std::cout << "[ACCESS] Condition held for "
                      << fmtDouble(held_seconds, 1)
                      << "s in current fail cycle. Queue Firebase log: "
                      << reason_to_log << std::endl;

            log_already_sent_in_cycle_ = true;
        }
    }

private:
    void startNewCycle(
        const std::chrono::steady_clock::time_point& now,
        bool is_rollover
    )
    {
        cycle_started_ = true;
        cycle_start_time_ = now;

        fake_timer_started_ = false;
        unknown_timer_started_ = false;
        log_already_sent_in_cycle_ = false;

        if (is_rollover)
        {
            std::cout << "[ACCESS] Continuous fail reached "
                      << fmtDouble(continuous_fail_cycle_seconds_, 1)
                      << "s. Start a new logging cycle." << std::endl;
        }
        else
        {
            std::cout << "[ACCESS] Start continuous fail cycle. Relog interval="
                      << fmtDouble(continuous_fail_cycle_seconds_, 1)
                      << "s" << std::endl;
        }
    }

    void updateConditionTimer(
        bool condition_active,
        bool& timer_started,
        std::chrono::steady_clock::time_point& start_time,
        const std::string& reason,
        const std::chrono::steady_clock::time_point& now
    )
    {
        if (condition_active)
        {
            if (!timer_started)
            {
                timer_started = true;
                start_time = now;

                std::cout << "[ACCESS] Start independent hold timer: "
                          << reason
                          << " | required="
                          << fmtDouble(required_hold_seconds_, 1)
                          << "s" << std::endl;
            }

            return;
        }

        // Chỉ điều kiện này mất liên tục mới reset timer của chính nó.
        timer_started = false;
    }

    static double elapsedTimerSeconds(
        bool timer_started,
        const std::chrono::steady_clock::time_point& start_time,
        const std::chrono::steady_clock::time_point& now
    )
    {
        if (!timer_started)
        {
            return 0.0;
        }

        return std::chrono::duration<double>(now - start_time).count();
    }

private:
    double required_hold_seconds_;
    double continuous_fail_cycle_seconds_;

    bool cycle_started_;
    bool fake_timer_started_;
    bool unknown_timer_started_;
    bool log_already_sent_in_cycle_;

    std::chrono::steady_clock::time_point cycle_start_time_;
    std::chrono::steady_clock::time_point fake_start_time_;
    std::chrono::steady_clock::time_point unknown_start_time_;
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
    LiveFaceBox live_box = {
        static_cast<float>(box.x1),
        static_cast<float>(box.y1),
        static_cast<float>(box.x2),
        static_cast<float>(box.y2)
    };
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

// ====================== ESP32 UART + UNLOCK GATE ======================
//
// Cơ chế tương đương code FaceNet Python:
// - UART /dev/serial0, baud 9600.
// - Gửi ASCII '1' khi REAL FACE + Known liên tục đủ 2 giây.
// - Sau khi gửi unlock, chờ 5 giây rồi mới bắt đầu đếm hợp lệ lại từ đầu.
// - Nếu tiếp tục REAL FACE + Known đủ 2 giây sau thời gian chờ,
//   gửi lại ASCII '1' để kích hoạt unlock lần mới.
// - Gửi ASCII '0' khi trạng thái không còn hợp lệ hoặc khi thoát.
//
class Esp32SerialController
{
public:
    Esp32SerialController(
        const std::string& device_path,
        int baud_rate
    )
        : device_path_(device_path),
          baud_rate_(baud_rate),
          fd_(-1),
          last_state_(-1)
    {
    }

    ~Esp32SerialController()
    {
        closePort();
    }

    bool openPort()
    {
        fd_ = ::open(
            device_path_.c_str(),
            O_RDWR | O_NOCTTY | O_NONBLOCK
        );

        if (fd_ < 0)
        {
            std::cerr << "[UART] Cannot open " << device_path_
                      << ": errno=" << errno << std::endl;
            return false;
        }

        struct termios tty;
        std::memset(&tty, 0, sizeof(tty));

        if (tcgetattr(fd_, &tty) != 0)
        {
            std::cerr << "[UART] tcgetattr failed: errno="
                      << errno << std::endl;
            closePort();
            return false;
        }

        speed_t baud_flag = B9600;

        if (baud_rate_ != 9600)
        {
            std::cerr << "[UART] Unsupported baud in this demo: "
                      << baud_rate_ << ". Use 9600." << std::endl;
            closePort();
            return false;
        }

        cfsetospeed(&tty, baud_flag);
        cfsetispeed(&tty, baud_flag);

        tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;
        tty.c_iflag &= ~IGNBRK;
        tty.c_lflag = 0;
        tty.c_oflag = 0;

        // Non-blocking read: Pi vẫn chạy camera/AI mượt, không bị đợi UART.
        tty.c_cc[VMIN] = 0;
        tty.c_cc[VTIME] = 0;

        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        tty.c_cflag |= (CLOCAL | CREAD);
        tty.c_cflag &= ~(PARENB | PARODD);
        tty.c_cflag &= ~CSTOPB;
#ifdef CRTSCTS
        tty.c_cflag &= ~CRTSCTS;
#endif

        if (tcsetattr(fd_, TCSANOW, &tty) != 0)
        {
            std::cerr << "[UART] tcsetattr failed: errno="
                      << errno << std::endl;
            closePort();
            return false;
        }

        tcflush(fd_, TCIOFLUSH);

        std::cout << "[UART] Connected to ESP32: "
                  << device_path_ << " @ " << baud_rate_ << " baud."
                  << std::endl;
        std::cout << "[UART] Pi TX sends '1'/'0'. Pi RX listens for '2'/'3'/'4'."
                  << std::endl;

        // Khởi động an toàn: báo đóng khóa trước.
        sendState(0, true);
        return true;
    }

    bool isOpened() const
    {
        return fd_ >= 0;
    }

    bool sendState(int state, bool force = false)
    {
        if (!isOpened())
        {
            return false;
        }

        const int normalized_state = (state == 1) ? 1 : 0;

        /*
         * Bình thường không gửi trùng trạng thái để tránh spam UART.
         * Với tín hiệu unlock lặp lại, ValidFaceUnlockGate gọi force=true
         * vì mỗi byte '1' là một lần trigger mở khóa mới cho ESP32.
         */
        if (!force && normalized_state == last_state_)
        {
            return true;
        }

        const char data = (normalized_state == 1) ? '1' : '0';
        const ssize_t written = ::write(fd_, &data, 1);

        if (written != 1)
        {
            std::cerr << "[UART] Write failed: errno="
                      << errno << std::endl;
            return false;
        }

        tcdrain(fd_);
        last_state_ = normalized_state;

        std::cout << "[UART] >>> "
                  << ((normalized_state == 1) ? "OPEN" : "CLOSE")
                  << " (sent '" << data << "')"
                  << (force ? " [force]" : "")
                  << std::endl;

        return true;
    }

    bool readCommand(char& command)
    {
        if (!isOpened())
        {
            return false;
        }

        // Đọc tối đa vài byte rác trong một lần gọi để tránh kẹt loop.
        for (int i = 0; i < 32; i++)
        {
            char data = 0;
            const ssize_t n = ::read(fd_, &data, 1);

            if (n == 1)
            {
                if (data == '2' || data == '3' || data == '4')
                {
                    command = data;
                    std::cout << "[UART] <<< ESP32 event '"
                              << command << "'" << std::endl;
                    return true;
                }

                // Nếu ESP32 dùng println(), bỏ qua xuống dòng.
                if (data == '\r' || data == '\n' || data == ' ' || data == '\t')
                {
                    continue;
                }

                std::cerr << "[UART] Ignore unknown byte from ESP32: '"
                          << data << "' (" << (int)(unsigned char)data << ")"
                          << std::endl;
                continue;
            }

            if (n == 0)
            {
                return false;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return false;
            }

            std::cerr << "[UART] Read failed: errno="
                      << errno << std::endl;
            return false;
        }

        return false;
    }

    void closePort()
    {
        if (fd_ >= 0)
        {
            sendState(0, true);
            ::close(fd_);
            fd_ = -1;
        }

        last_state_ = -1;
    }

private:
    std::string device_path_;
    int baud_rate_;
    int fd_;
    int last_state_;
};

class ValidFaceUnlockGate
{
public:
    ValidFaceUnlockGate(
        double required_valid_seconds,
        double recheck_delay_seconds
    )
        : required_valid_seconds_(std::max(0.1, required_valid_seconds)),
          recheck_delay_seconds_(std::max(0.0, recheck_delay_seconds)),
          valid_timer_started_(false),
          waiting_for_recheck_(false),
          unlock_sent_in_session_(false),
          last_face_unlock_sent_(false)
    {
    }

    void observe(
        const CachedResult& cached,
        bool suppress_unlock,
        Esp32SerialController& serial
    )
    {
        const bool valid_access =
            !suppress_unlock &&
            cached.has_face &&
            cached.is_real &&
            cached.match.known;

        /*
         * Chỉ cần mất một trong ba điều kiện REAL + FACE + Known:
         * - hủy thời gian chờ/đếm hiện tại;
         * - nếu phiên này từng mở khóa thì gửi '0';
         * - lần hợp lệ tiếp theo sẽ đếm lại từ đầu.
         */
        if (!valid_access)
        {
            resetAndLock(serial);
            return;
        }

        const auto now = std::chrono::steady_clock::now();

        /*
         * Sau khi đã gửi unlock, không đếm tiếp ngay.
         * Đợi đủ 5 giây rồi mới bắt đầu một bộ đếm hợp lệ 2 giây mới.
         */
        if (waiting_for_recheck_)
        {
            const double waited_seconds =
                std::chrono::duration<double>(
                    now - last_unlock_time_
                ).count();

            if (waited_seconds < recheck_delay_seconds_)
            {
                return;
            }

            waiting_for_recheck_ = false;
            valid_timer_started_ = true;
            valid_start_time_ = now;

            std::cout << "[UNLOCK] Recheck delay completed after "
                      << fmtDouble(waited_seconds, 1)
                      << "s. Start a new valid-face timer for user="
                      << cached.match.name
                      << " | required="
                      << fmtDouble(required_valid_seconds_, 1)
                      << "s" << std::endl;
            return;
        }

        if (!valid_timer_started_)
        {
            valid_timer_started_ = true;
            valid_start_time_ = now;

            std::cout << "[UNLOCK] Start valid-face timer: "
                      << cached.match.name
                      << " | required="
                      << fmtDouble(required_valid_seconds_, 1)
                      << "s" << std::endl;
            return;
        }

        const double valid_seconds =
            std::chrono::duration<double>(now - valid_start_time_).count();

        if (valid_seconds < required_valid_seconds_)
        {
            return;
        }

        /*
         * Luôn force gửi byte '1':
         * Sau lần mở đầu tiên, last_state_ của UART vẫn là OPEN (1).
         * Nếu không force, sendState() sẽ bỏ qua lần unlock kế tiếp.
         */
        if (serial.sendState(1, true))
        {
            unlock_sent_in_session_ = true;
            waiting_for_recheck_ = true;
            valid_timer_started_ = false;
            last_unlock_time_ = now;

            // Lưu context để khi ESP32 phản hồi '3', Pi log đúng user face granted.
            last_face_unlock_sent_ = true;
            last_face_unlock_time_ = now;
            last_face_unlock_user_ = cached.match.name.empty()
                ? "Unknown"
                : cached.match.name;

            std::cout << "[UNLOCK] Access valid for "
                      << fmtDouble(valid_seconds, 1)
                      << "s. ESP32 unlock sent for user="
                      << cached.match.name
                      << ". Recheck countdown will restart after "
                      << fmtDouble(recheck_delay_seconds_, 1)
                      << "s." << std::endl;
        }
    }

    bool consumeRecentFaceUnlockAck(
        const std::chrono::steady_clock::time_point& now,
        double max_ack_age_seconds,
        std::string& user_name
    )
    {
        if (!last_face_unlock_sent_)
        {
            return false;
        }

        const double age_seconds =
            std::chrono::duration<double>(
                now - last_face_unlock_time_
            ).count();

        if (age_seconds > max_ack_age_seconds)
        {
            last_face_unlock_sent_ = false;
            return false;
        }

        user_name = last_face_unlock_user_.empty()
            ? "Unknown"
            : last_face_unlock_user_;
        last_face_unlock_sent_ = false;
        return true;
    }

    void resetAndLock(Esp32SerialController& serial)
    {
        valid_timer_started_ = false;
        waiting_for_recheck_ = false;

        if (unlock_sent_in_session_)
        {
            serial.sendState(0);
        }

        unlock_sent_in_session_ = false;
    }

    void shutdown(Esp32SerialController& serial)
    {
        valid_timer_started_ = false;
        waiting_for_recheck_ = false;
        unlock_sent_in_session_ = false;
        last_face_unlock_sent_ = false;
        serial.sendState(0, true);
    }

private:
    double required_valid_seconds_;
    double recheck_delay_seconds_;

    bool valid_timer_started_;
    bool waiting_for_recheck_;
    bool unlock_sent_in_session_;

    // Context cho phản hồi '3' từ ESP32 sau face unlock.
    bool last_face_unlock_sent_;
    std::string last_face_unlock_user_;

    std::chrono::steady_clock::time_point valid_start_time_;
    std::chrono::steady_clock::time_point last_unlock_time_;
    std::chrono::steady_clock::time_point last_face_unlock_time_;
};

class Esp32FeedbackLogGate
{
public:
    explicit Esp32FeedbackLogGate(double debounce_seconds)
        : debounce_seconds_(std::max(0.0, debounce_seconds)),
          has_last_event_(false),
          last_command_(0)
    {
    }

    void observe(
        char command,
        const cv::Mat& snapshot_frame,
        const CachedResult& cached,
        ValidFaceUnlockGate& valid_face_unlock_gate,
        FirestoreLogger& logger
    )
    {
        if (command != '2' && command != '3' && command != '4')
        {
            return;
        }

        const auto now = std::chrono::steady_clock::now();

        if (has_last_event_ && command == last_command_)
        {
            const double dt =
                std::chrono::duration<double>(now - last_event_time_).count();

            if (dt < debounce_seconds_)
            {
                std::cout << "[ESP32_EVENT] Debounce duplicate command '"
                          << command << "' after "
                          << fmtDouble(dt, 2) << "s" << std::endl;
                return;
            }
        }

        std::string method;
        std::string result;
        std::string reason;
        std::string user_name;

        if (command == '2')
        {
            // ESP32 báo nhập sai PIN.
            method = "keypad";
            result = "DENIED";
            reason = "WRONG_PIN";
            user_name = "Unknown";
        }
        else if (command == '3')
        {
            // ESP32 xác nhận mở khóa do face unlock từ Pi.
            method = "face";
            result = "GRANTED";
            reason = "ACCESS_ACCEPTED";

            std::string face_user;
            const bool has_recent_face_context =
                valid_face_unlock_gate.consumeRecentFaceUnlockAck(
                    now,
                    FACE_GRANTED_CONTEXT_WINDOW_SECONDS,
                    face_user
                );

            if (has_recent_face_context && !face_user.empty())
            {
                user_name = face_user;
            }
            else if (cached.has_face && cached.is_real && cached.match.known &&
                     !cached.match.name.empty())
            {
                user_name = cached.match.name;
            }
            else
            {
                user_name = "Unknown";
            }
        }
        else // command == '4'
        {
            // ESP32 xác nhận mở khóa do nhập đúng keypad PIN.
            method = "keypad";
            result = "GRANTED";
            reason = "ACCESS_ACCEPTED";
            user_name = "KeypadUser";
        }

        if (logger.enqueueAccessEvent(
                method,
                result,
                reason,
                user_name,
                snapshot_frame,
                cached))
        {
            has_last_event_ = true;
            last_command_ = command;
            last_event_time_ = now;

            std::cout << "[ESP32_EVENT] Queue Firestore log: "
                      << "cmd='" << command << "'"
                      << " | method=" << method
                      << " | result=" << result
                      << " | reason=" << reason
                      << " | userName=" << user_name
                      << std::endl;
        }
    }

private:
    double debounce_seconds_;
    bool has_last_event_;
    char last_command_;
    std::chrono::steady_clock::time_point last_event_time_;
};


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
    ensureDir(ACCESS_LOG_IMAGE_DIR);

    std::cout << "[INFO] LOG IMAGE DIR = "
              << ACCESS_LOG_IMAGE_DIR << std::endl;

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

    FirestoreLogger firestore_logger;
    PiDeniedEventGate firestore_event_gate(
        FIRESTORE_DENIED_HOLD_SECONDS,
        FIRESTORE_CONTINUOUS_FAIL_CYCLE_SECONDS
    );

    Esp32SerialController esp32_serial(
        ESP32_SERIAL_PORT,
        ESP32_SERIAL_BAUD
    );
    ValidFaceUnlockGate valid_face_unlock_gate(
        VALID_FACE_UNLOCK_HOLD_SECONDS,
        UNLOCK_RECHECK_DELAY_SECONDS
    );
    Esp32FeedbackLogGate esp32_feedback_log_gate(
        ESP32_FEEDBACK_LOG_DEBOUNCE_SECONDS
    );

    bool esp32_serial_ready = false;
    if (ENABLE_ESP32_UNLOCK_SIGNAL)
    {
        esp32_serial_ready = esp32_serial.openPort();

        if (!esp32_serial_ready)
        {
            std::cerr << "[UART] Vision and Firestore continue, "
                      << "but ESP32 unlock output is OFF." << std::endl;
        }
    }

    if (ENABLE_FIRESTORE_LOG && !firestore_logger.start())
    {
        std::cerr << "[FIRESTORE] Vision still runs, but cloud logging is OFF."
                  << std::endl;
    }
    else if (ENABLE_FIRESTORE_LOG)
    {
        std::cout << "[FIRESTORE] Independent denied timers enabled: "
                  << "FAKE or UNKNOWN held for "
                  << fmtDouble(FIRESTORE_DENIED_HOLD_SECONDS, 1)
                  << " seconds will create one log." << std::endl;
        std::cout << "[FIRESTORE] Continuous failure relog cycle: "
                  << fmtDouble(FIRESTORE_CONTINUOUS_FAIL_CYCLE_SECONDS, 1)
                  << " seconds." << std::endl;
        std::cout << "[FIRESTORE] ESP32 feedback logs: "
                  << "'2'=WRONG_PIN, "
                  << "'3'=FACE_ACCESS_ACCEPTED, "
                  << "'4'=KEYPAD_ACCESS_ACCEPTED. "
                  << "Snapshots are saved locally too."
                  << std::endl;
    }

    if (ENABLE_ESP32_UNLOCK_SIGNAL)
    {
        std::cout << "[UNLOCK] Requirement: REAL FACE + Known continuously for "
                  << fmtDouble(VALID_FACE_UNLOCK_HOLD_SECONDS, 1)
                  << " seconds -> send ASCII '1'. After each unlock, wait "
                  << fmtDouble(UNLOCK_RECHECK_DELAY_SECONDS, 1)
                  << " seconds, then count valid time again for the next unlock."
                  << std::endl;
    }

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

        bool has_new_inference_result = false;
        bool add_user_was_open_for_inference = add_state.window_open;

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
            has_new_inference_result = true;
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

        if (has_new_inference_result &&
            ENABLE_ESP32_UNLOCK_SIGNAL &&
            esp32_serial_ready)
        {
            bool suppress_unlock =
                add_state.window_open || add_user_was_open_for_inference;

            valid_face_unlock_gate.observe(
                cached,
                suppress_unlock,
                esp32_serial
            );
        }

        if (ENABLE_ESP32_UNLOCK_SIGNAL && esp32_serial_ready)
        {
            char esp32_command = 0;
            int read_count = 0;

            while (read_count < ESP32_UART_MAX_READ_PER_LOOP &&
                   esp32_serial.readCommand(esp32_command))
            {
                read_count++;

                if (ENABLE_FIRESTORE_LOG && firestore_logger.isEnabled())
                {
                    esp32_feedback_log_gate.observe(
                        esp32_command,
                        display_frame,
                        cached,
                        valid_face_unlock_gate,
                        firestore_logger
                    );
                }
                else
                {
                    std::cout << "[ESP32_EVENT] Received command '"
                              << esp32_command
                              << "' but Firestore logger is OFF."
                              << std::endl;
                }
            }
        }

        if (has_new_inference_result &&
            ENABLE_FIRESTORE_LOG &&
            firestore_logger.isEnabled())
        {
            bool suppress_firestore_logging =
                add_state.window_open || add_user_was_open_for_inference;

            firestore_event_gate.observe(
                cached,
                display_frame,
                suppress_firestore_logging,
                firestore_logger
            );
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

    if (ENABLE_ESP32_UNLOCK_SIGNAL && esp32_serial_ready)
    {
        valid_face_unlock_gate.shutdown(esp32_serial);
        // closePort() tự gửi ASCII '0' trước khi đóng UART.
        esp32_serial.closePort();
    }

    firestore_logger.stop();
    cv::destroyAllWindows();

    std::cout << "[INFO] CSI camera stopped." << std::endl;

    return 0;
}