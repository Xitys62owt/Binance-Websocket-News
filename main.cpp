#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/deadline_timer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = boost::asio::ssl;
using tcp = boost::asio::ip::tcp;
using namespace std::chrono_literals;

// Структура для хранения данных анонса
struct BinanceAnnouncement {
  int catalogId;
  std::string catalogName;
  long publishDate;
  std::string title;
  std::string body;
  std::string disclaimer;

  void print() const {
    std::time_t time = publishDate / 1000;
    std::tm local_time_storage;
#ifdef _WIN32
    localtime_s(&local_time_storage, &time);
#else
    localtime_r(&time, &local_time_storage);
#endif
    char time_buf[80];
    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S",
                  &local_time_storage);

    std::cout << "\n--- Binance Announcement ---\n"
              << "catalogId: " << catalogId << "\n"
              << "catalogName: " << catalogName << "\n"
              << "publishDate: " << time_buf << "\n"
              << "title: " << title << "\n"
              << "body: " << body << "\n"
              << "disclaimer: " << disclaimer << "\n"
              << "------------------------------\n";
  }
};

class BinanceWebSocketClient {
 public:
  BinanceWebSocketClient(const std::string& api_key,
                         const std::string& secret_key)
      : api_key_(api_key),
        secret_key_(secret_key),
        resolver_(io_context_),
        ssl_ctx_(ssl::context::tlsv12_client),
        reconnect_timer_(io_context_),
        ping_timer_(io_context_) {
    last_reset_time_ = std::chrono::steady_clock::now();
    ssl_ctx_.set_default_verify_paths();
    ssl_ctx_.set_verify_mode(ssl::verify_peer);
  }

  void run() {
    std::cout << "Клиент запущен. Начало подключения" << std::endl;
    io_thread_ = std::thread([this] {
      net::post(io_context_, [this] { connect(); });
      io_context_.run();
    });
  }

  void stop() {
    stop_requested_ = true;
    net::post(io_context_, [this] {
      if (ws_ && ws_->is_open()) {
        beast::error_code ec;
        ws_->close(websocket::close_code::normal, ec);
      }
      reconnect_timer_.cancel();
      ping_timer_.cancel();
    });

    if (io_thread_.joinable()) {
      io_thread_.join();
    }
  }

 private:
  std::string api_key_;
  std::string secret_key_;
  net::io_context io_context_;
  std::thread io_thread_;
  tcp::resolver resolver_;
  ssl::context ssl_ctx_;
  std::unique_ptr<websocket::stream<beast::ssl_stream<tcp::socket>>> ws_;
  std::atomic<bool> stop_requested_{false};
  std::chrono::steady_clock::time_point connection_start_time_;
  std::atomic<int> message_counter_{0};
  std::chrono::steady_clock::time_point last_reset_time_;
  boost::asio::deadline_timer reconnect_timer_;
  boost::asio::deadline_timer ping_timer_;
  std::chrono::steady_clock::time_point last_ping_log_time_;
  beast::flat_buffer read_buffer_;

  void connect() {
    if (stop_requested_) return;
    // Отключение при необходимости
    reconnect_timer_.cancel();
    ping_timer_.cancel();
    if (ws_ && ws_->is_open()) {
      beast::error_code ec;
      ws_->close(websocket::close_code::normal, ec);
    }
    ws_.reset();

    try {
      // Параметры подключения
      const auto random_str = generate_random(32);
      const auto timestamp = std::to_string(get_current_timestamp());
      const std::string topic = "com_announcement_en";
      const long recv_window = 50000;
      const std::string signature =
          generate_signature(random_str, topic, recv_window, timestamp);
      std::string query = "random=" + random_str + "&topic=" + topic +
                          "&recvWindow=" + std::to_string(recv_window) +
                          "&timestamp=" + timestamp + "&signature=" + signature;

      auto const results = resolver_.resolve("api.binance.com", "443");

      // Создание сокета
      ws_ = std::make_unique<websocket::stream<beast::ssl_stream<tcp::socket>>>(
          io_context_, ssl_ctx_);
      ws_->set_option(websocket::stream_base::decorator(
          [this](websocket::request_type& req) {
            req.set("X-MBX-APIKEY", api_key_);
          }));

      // Подключение
      beast::get_lowest_layer(*ws_).connect(*results.begin());
      ws_->next_layer().handshake(ssl::stream_base::client);
      ws_->handshake("api.binance.com", "/sapi/wss?" + query);

      connection_start_time_ = std::chrono::steady_clock::now();
      std::cout << "Подключение к сокету успешно" << std::endl;

      // Подписка
      std::string subscribe_msg = R"({
                "command": "SUBSCRIBE",
                "value": "com_announcement_en"
            })";
      ws_->write(net::buffer(subscribe_msg));

      // Запуск таймеров и чтения
      last_ping_log_time_ = std::chrono::steady_clock::now();
      start_reconnect_timer();
      start_ping_timer();
      start_read();
    } catch (const std::exception& e) {
      std::cerr << "Ошибка подключения: " << e.what() << ". Повтор через 5с"
                << std::endl;
      reconnect_timer_.expires_from_now(boost::posix_time::seconds(5));
      reconnect_timer_.async_wait([this](const boost::system::error_code& ec) {
        if (!ec) {
          connect();
        }
      });
    }
  }

  // Переподключение раз в 24 часа (стоит на 22 на всякий)
  void start_reconnect_timer() {
    reconnect_timer_.expires_from_now(boost::posix_time::minutes(1420));
    reconnect_timer_.async_wait([this](const boost::system::error_code& ec) {
      if (!ec && !stop_requested_) {
        std::cout << "Плановое переподключение" << std::endl;
        connect();
      }
    });
  }

  // Ping-поток 
  void start_ping_timer() {
    ping_timer_.expires_from_now(boost::posix_time::seconds(30));
    ping_timer_.async_wait([this](const boost::system::error_code& ec) {
      if (ec || stop_requested_) return;

      check_rate_limit();

      if (ws_ && ws_->is_open()) {
        ws_->ping({});
        ++message_counter_;
        
        auto now = std::chrono::steady_clock::now();
        if (now - last_ping_log_time_ > 30min) {
            auto system_now = std::chrono::system_clock::now();
            std::time_t time = std::chrono::system_clock::to_time_t(system_now);
            std::tm local_time_storage;
#ifdef _WIN32
            localtime_s(&local_time_storage, &time);
#else
            localtime_r(&time, &local_time_storage);
#endif
            char time_buf[80];
            std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &local_time_storage);
                
            std::cout << "PING отправлен " << time_buf << std::endl;
            last_ping_log_time_ = now;
        }
      }

      start_ping_timer();
    });
  }

  // Поток чтения
  void start_read() {
    if (stop_requested_ || !ws_) return;

    ws_->async_read(read_buffer_, [this](beast::error_code ec, size_t) {
      if (ec) {
        // Плановое переподключение
        if (ec == net::error::operation_aborted || stop_requested_) {
          return;
        }

        if (ec == websocket::error::closed) {
          std::cout << "Соединение закрыто сервером" << std::endl;
        } else {
          std::cerr << "Ошибка чтения: " << ec.message() << std::endl;
        }

        connect();
        return;
      }

      // Обработка сообщения
      std::string message = beast::buffers_to_string(read_buffer_.data());
      read_buffer_.consume(read_buffer_.size());
      handle_message(message);

      // Запуск следующего чтения
      if (!stop_requested_) {
        start_read();
      }
    });
  }

  std::string generate_random(size_t length) {
    static const char alphabetnum[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string result;
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, sizeof(alphabetnum) - 2);

    for (size_t i = 0; i < length; ++i) {
      result += alphabetnum[dis(gen)];
    }
    return result;
  }

  long get_current_timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  }

  // Генерация подписи HMAC-SHA256
  std::string generate_signature(const std::string& random,
                                 const std::string& topic, long recv_window,
                                 const std::string& timestamp) {
    std::string payload = "random=" + random + "&topic=" + topic +
                          "&recvWindow=" + std::to_string(recv_window) +
                          "&timestamp=" + timestamp;

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int length = 0;

    HMAC_CTX* ctx = HMAC_CTX_new();
    HMAC_Init_ex(ctx, secret_key_.c_str(), secret_key_.length(), EVP_sha256(),
                 nullptr);
    HMAC_Update(ctx, reinterpret_cast<const unsigned char*>(payload.c_str()),
                payload.length());
    HMAC_Final(ctx, hash, &length);
    HMAC_CTX_free(ctx);

    std::stringstream ss;
    for (unsigned int i = 0; i < length; ++i) {
      ss << std::hex << std::setw(2) << std::setfill('0')
         << static_cast<int>(hash[i]);
    }

    return ss.str();
  }

  // Функция проверки лимита 5 сообщений в секунду
  void check_rate_limit() {
    auto now = std::chrono::steady_clock::now();

    if (now - last_reset_time_ >= 1s) {
      message_counter_ = 0;
      last_reset_time_ = now;
    }

    // Счетчик не првеосходит 4, чтобы предвосхитить отправленный бинансом понг
    if (message_counter_ >= 4) {
      auto wait_time = std::chrono::duration_cast<std::chrono::milliseconds>(
          1s - (now - last_reset_time_));

      if (wait_time.count() > 0) {
        std::cout << "Достигнут лимит сообщений, ожидание " << wait_time.count()
                  << "ms" << std::endl;
        std::this_thread::sleep_for(wait_time);
      }

      message_counter_ = 0;
      last_reset_time_ = std::chrono::steady_clock::now();
    }
  }

  // Обработка входящих сообщений
  void handle_message(const std::string& message) {
    rapidjson::Document doc;
    doc.Parse(message.c_str());

    if (doc.HasParseError()) {
      std::cerr << "Ошибка парсинга JSON: "
                << rapidjson::GetParseError_En(doc.GetParseError())
                << std::endl;
      return;
    }

    if (doc.HasMember("type") && doc["type"].IsString()) {
      const std::string type = doc["type"].GetString();

      if (type == "DATA" && doc.HasMember("topic") && doc["topic"].IsString()) {
        const std::string topic = doc["topic"].GetString();
        if (topic == "com_announcement_en" && doc.HasMember("data") &&
            doc["data"].IsString()) {
          handle_announcement(doc["data"].GetString());
        }
      } else if (type == "COMMAND") {
        if (doc.HasMember("subType") && doc["subType"].IsString() &&
            doc.HasMember("data") && doc["data"].IsString()) {
          const std::string subType = doc["subType"].GetString();
          const std::string data = doc["data"].GetString();

          if (subType == "SUBSCRIBE" && data == "SUCCESS") {
            std::cout << "Подписка на com_announcement_en успешна" << std::endl;
          } else {
            std::cerr << "Ошибка подписки: " << message << std::endl;
          }
        }
      }
    }
  }

  // Обработка анонса
  void handle_announcement(const std::string& json_data) {
    rapidjson::Document doc;
    doc.Parse(json_data.c_str());

    if (doc.HasParseError()) {
      std::cerr << "Ошибка парсинга JSON анонса: "
                << rapidjson::GetParseError_En(doc.GetParseError())
                << std::endl;
      return;
    }

    BinanceAnnouncement announcement;

    if (doc.HasMember("catalogId") && doc["catalogId"].IsInt())
      announcement.catalogId = doc["catalogId"].GetInt();
    if (doc.HasMember("catalogName") && doc["catalogName"].IsString())
      announcement.catalogName = doc["catalogName"].GetString();
    if (doc.HasMember("publishDate") && doc["publishDate"].IsInt64())
      announcement.publishDate = doc["publishDate"].GetInt64();
    if (doc.HasMember("title") && doc["title"].IsString())
      announcement.title = doc["title"].GetString();
    if (doc.HasMember("body") && doc["body"].IsString())
      announcement.body = doc["body"].GetString();
    if (doc.HasMember("disclaimer") && doc["disclaimer"].IsString())
      announcement.disclaimer = doc["disclaimer"].GetString();

    announcement.print();
  }
};

int main() {
  const std::string api_key =
      "nsFokStrfthrJgkw6LXvlXXZan7OyjbGqjmGkcJJZrqkDzn6aMPVPMyCsNIt9zQl";
  const std::string secret_key =
      "6Ek4vQsQBoSMyjiModuV4CGHwa7PfyrKX31ZZY4X6uX1if3VodXJ2aazhS2PWrnZ";

  try {
    BinanceWebSocketClient client(api_key, secret_key);
    client.run();

    std::cout << "Сервис запущен. Нажмите Enter для выхода" << std::endl;
    std::cin.get();

    client.stop();
    std::cout << "Сервис остановлен" << std::endl;
  } catch (const std::exception& e) {
    std::cerr << "Фатальная ошибка: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}

