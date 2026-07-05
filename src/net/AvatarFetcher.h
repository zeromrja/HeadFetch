#pragma once

#include "../core/Types.h"
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <curl/curl.h>
#include <deque>
#include <mutex>
#include <stb_image.h>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#define HF_LOG(fmt, ...) \
std::printf("[AvatarFetcher] " fmt "\n", ##__VA_ARGS__)

namespace hf::Net {

// Queues xuid -> uuid lookups and resolves them on a single background
// thread, so the game/render threads never block on network I/O.
//
// Flow per player:
//   1. GET .../profile/xuid/<xuid>/image/avatar
//      - not a 200 with a PNG body -> stop here, normal (local skin crop)
//        flow is left completely untouched.
//   2. Only if step 1 succeeded: GET .../profile/xuid/<xuid>/image/head,
//      decode it, and push it into State::PendingHeads so it replaces the
//      locally-cropped head texture for that uuid.
class AvatarFetcher {
public:
	static AvatarFetcher& instance(){
		static AvatarFetcher inst;
		return inst;
	}

	// Starts the worker thread. Safe to call more than once.
	void start(){
		if(m_started.exchange(true)){
			HF_LOG("Already started");
			return;
		}

		HF_LOG("Starting worker thread");
		curl_global_init(CURL_GLOBAL_DEFAULT);
		m_worker = std::thread([this]{ run(); });
	}

	// Queues an avatar/head lookup for a player. Cheap to call on every
	// PlayerListPacket entry: requests are de-duplicated internally, so a
	// given uuid is only ever looked up once until forget() is called.
	void request(const std::string& uuidStr, const std::string& xuid){
		if(xuid.empty() || xuid.size() > 32){
			HF_LOG("Rejected request for uuid=%s (invalid xuid)", uuidStr.c_str());
			return;
		}

		std::lock_guard<std::mutex> lock(m_mutex);

		if(m_seen.contains(uuidStr)){
			HF_LOG("Already queued/resolved uuid=%s", uuidStr.c_str());
			return;
		}

		HF_LOG("Queued uuid=%s xuid=%s", uuidStr.c_str(), xuid.c_str());

		m_seen.insert(uuidStr);
		m_queue.push_back({uuidStr, xuid});
		m_cv.notify_one();
	}

	// Call when a player leaves the tab list so they're re-checked if they
	// rejoin (e.g. if they add a Bedrock avatar later in the session).
	void forget(const std::string& uuidStr){
		std::lock_guard<std::mutex> lock(m_mutex);
		m_seen.erase(uuidStr);
	}

private:
	AvatarFetcher() = default;

	struct Request {
		std::string uuidStr;
		std::string xuid;
	};

	struct Buffer {
		std::vector<std::uint8_t> data;
	};

	static constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;

	static std::size_t writeCb(char* ptr, std::size_t size, std::size_t nmemb, void* userdata){
		auto* buf = static_cast<Buffer*>(userdata);
		auto total = size * nmemb;
		if(buf->data.size() + total > kMaxResponseBytes){ return 0; } // abort: response too large
		buf->data.insert(buf->data.end(), ptr, ptr + total);
		return total;
	}

	// Fetches a URL fully into memory. Returns true only for a plain HTTP
	// 200 whose body starts with the PNG magic bytes -- this is the "did
	// the endpoint actually respond with an image" check.
	static bool fetchPng(CURL* curl, const std::string& url, std::vector<std::uint8_t>& out){
		Buffer buf;
		curl_easy_reset(curl);
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &AvatarFetcher::writeCb);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
		curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
		curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
		curl_easy_setopt(curl, CURLOPT_USERAGENT, "headfetch/1.0");
		curl_easy_setopt(curl, CURLOPT_CAINFO, "/etc/ssl/certs/ca-certificates.crt");

		HF_LOG("GET %s", url.c_str());

		auto res = curl_easy_perform(curl);

		HF_LOG("curl_easy_perform = %d (%s)",
			   (int)res,
			   curl_easy_strerror(res));

		long code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

		char* ct = nullptr;
		curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &ct);

		HF_LOG("HTTP = %ld", code);
		HF_LOG("Content-Type = %s", ct ? ct : "(null)");
		HF_LOG("Downloaded = %zu bytes", buf.data.size());

		if(res != CURLE_OK){
			return false;
		}

		HF_LOG("HTTP %ld (%zu bytes)", code, buf.data.size());

		if(code != 200){
			HF_LOG("Server returned HTTP %ld", code);
			return false;
		}

		static constexpr std::uint8_t pngMagic[8] = {
			0x89,'P','N','G','\r','\n',0x1a,'\n'
		};

		if(buf.data.size() < sizeof(pngMagic)){
			HF_LOG("Response too small");
			return false;
		}

		if(std::memcmp(buf.data.data(), pngMagic, sizeof(pngMagic)) != 0){
			HF_LOG("Response is not PNG");
			return false;
		}

		HF_LOG("PNG OK");

		out = std::move(buf.data);
		return true;
	}

	static bool decodeToHead(const std::vector<std::uint8_t>& png, HeadPixels& out){
		int w = 0, h = 0, channels = 0;
		auto* pixels = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &channels, 4);
		if(!pixels){
			HF_LOG("stbi_load_from_memory failed");
			return false;
		}

		HF_LOG("Decoded image %dx%d", w, h);
		if(w <= 0 || h <= 0){ stbi_image_free(pixels); return false; }

		// Nearest-neighbour resample into the fixed HEAD_TEX_SIZE square,
		// since the API can return different resolutions.
		for(int y = 0; y < HEAD_TEX_SIZE; ++y){
			int sy = y * h / HEAD_TEX_SIZE;
			for(int x = 0; x < HEAD_TEX_SIZE; ++x){
				int sx = x * w / HEAD_TEX_SIZE;
				const auto* src = pixels + (static_cast<std::size_t>(sy) * w + sx) * 4;
				auto* dst = out.data() + (y * HEAD_TEX_SIZE + x) * 4;
				std::memcpy(dst, src, 4);
			}
		}
		stbi_image_free(pixels);
		return true;
	}

	void run(){
		CURL* curl = curl_easy_init();
		if(!curl){ printf("[headfetch] curl_easy_init failed, avatar fetch disabled\n"); return; }

		while(true){
			Request req;
			{
				std::unique_lock<std::mutex> lock(m_mutex);
				m_cv.wait(lock, [this]{ return m_stop || !m_queue.empty(); });
				if(m_stop && m_queue.empty()){ break; }
				req = std::move(m_queue.front());
				m_queue.pop_front();
				HF_LOG("Processing uuid=%s xuid=%s",
					   req.uuidStr.c_str(),
					   req.xuid.c_str());
			}

			const std::string avatarUrl =
				"https://persona.franchise.minecraft-services.net/api/v1.0/profile/xuid/"
				+ req.xuid + "/image/avatar";

			std::vector<std::uint8_t> avatarBytes;
			if(!fetchPng(curl, avatarUrl, avatarBytes)){
				// Endpoint didn't answer with a png -- leave the normal,
				// locally-cropped-skin flow completely alone.
				HF_LOG("Avatar not found (xuid=%s)", req.xuid.c_str());
				continue;
			}
			HF_LOG("Avatar exists");

			const std::string headUrl =
				"https://persona.franchise.minecraft-services.net/api/v1.0/profile/xuid/"
				+ req.xuid + "/image/head";

			std::vector<std::uint8_t> headBytes;
			if(!fetchPng(curl, headUrl, headBytes)){
				HF_LOG("Head image not found");
				continue; }

			HeadPixels pixels{};
			if(!decodeToHead(headBytes, pixels)){
				HF_LOG("Failed to decode head");
				continue; }

			{
				std::lock_guard<std::mutex> lock(State::PendingHeadsMutex);
				State::PendingHeads[req.uuidStr] = pixels;
				HF_LOG("Stored head for uuid=%s", req.uuidStr.c_str());
			}
			{
				std::lock_guard<std::mutex> lock(State::RemoteResolvedMutex);
				State::RemoteResolved.insert(req.uuidStr);
			}
		}
		HF_LOG("Worker exiting");
		curl_easy_cleanup(curl);
	}

	std::atomic<bool> m_started{false};
	std::thread m_worker;
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::deque<Request> m_queue;
	std::unordered_set<std::string> m_seen;
	bool m_stop = false;
};

} // namespace hf::Net
