#pragma once
// shared types
#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hf {

inline constexpr int HEAD_TEX_SIZE = 64;
using HeadPixels = std::array<std::uint8_t, HEAD_TEX_SIZE * HEAD_TEX_SIZE * 4>;

struct PlayerInfo {
	std::string name;
	std::string uuid;
};

struct UUIDRaw { std::uint64_t a, b; };
inline bool operator==(const UUIDRaw& l, const UUIDRaw& r){
	return l.a == r.a && l.b == r.b;
}

struct CachedPlayer {
	UUIDRaw     uuid;
	std::string uuidString;
	std::string name;
};

namespace State {
	inline std::vector<PlayerInfo>  Players;
	inline std::mutex               PlayersMutex;
	inline std::unordered_map<std::string, HeadPixels> PendingHeads;
	inline std::mutex               PendingHeadsMutex;

	// UUIDs (as string) whose head texture came from the network avatar
	// endpoint. While a uuid is in this set, the locally-cropped skin head
	// is not allowed to overwrite it back in PendingHeads.
	inline std::unordered_set<std::string> RemoteResolved;
	inline std::mutex               RemoteResolvedMutex;
	inline std::atomic<bool>        TabHeld{false};
	inline std::atomic<double>      LastHudTime{0.0};

	inline double monotonicNow(){
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (double)ts.tv_sec + ts.tv_nsec / 1e9;
	}
}

} // namespace hf
