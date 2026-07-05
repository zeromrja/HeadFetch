#pragma once

#include "../core/Scanner.h"
#include "../core/Types.h"
#include "../game/Offsets.h"
#include "../net/AvatarFetcher.h"
#include "../skin/SkinCropper.h"
#include "../skin/TextureCache.h"
#include "ImGuiLayer.h"
#include <EGL/egl.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace hf {

class PlayerBoard {
public:
	static PlayerBoard& instance(){
		static PlayerBoard inst;
		return inst;
	}

	void preinit(){
		using PreinitHook = void(*)(const char*, void*, void**);
		auto h = dlopen("libmcpelauncher_mod.so", 0);
		if(!h){ printf("[headfetch] libmcpelauncher_mod.so not found\n"); return; }
		auto hook = (PreinitHook)dlsym(h, "mcpelauncher_preinithook");
		if(hook){
			hook("eglSwapBuffers", (void*)&eglSwapBuffers_, (void**)&s_swapOrig);
			hook("eglMakeCurrent", (void*)&eglMakeCurrent_, (void**)&s_makeCurrentOrig);
			printf("[headfetch] EGL hooks registered\n");
		}
		dlclose(h);
	}

	void init(){
		resolveEGL();
		installHooks();
		installKeyboard();
		Net::AvatarFetcher::instance().start();
	}

	void onHudRender(void* ctx, void* client){
		State::LastHudTime = State::monotonicNow();
	}

	void onPacketRead(void* packet){
		if(!packet){ return; }
		auto addr = reinterpret_cast<std::uintptr_t>(packet);
		auto begin  = *reinterpret_cast<std::uintptr_t*>(addr + Game::PLP_EntriesBegin);
		auto end    = *reinterpret_cast<std::uintptr_t*>(addr + Game::PLP_EntriesEnd);
		auto action = *reinterpret_cast<std::uint8_t*>(addr + Game::PLP_Action);
		if(!begin || !end || end < begin){ return; }
		auto bytes = end - begin;
		if(bytes % Game::PLP_EntrySize != 0){ return; }
		auto count = bytes / Game::PLP_EntrySize;
		if(count == 0 || count > 300){ return; }
		std::lock_guard<std::mutex> lock(State::PlayersMutex);
		if(action == 0 && count > 1){ m_cache.clear(); }
		for(std::size_t i = 0; i < count; ++i){
			auto entry = begin + i * Game::PLP_EntrySize;
			auto uuid  = *reinterpret_cast<const UUIDRaw*>(entry + Game::PLP_EntryUuid);
			if(action == 0){
				auto* nameStr = reinterpret_cast<const std::string*>(entry + Game::PLP_EntryName);
				if(nameStr && !nameStr->empty() && nameStr->size() <= 64){
					auto uuidStr = uuidToString(uuid);

					bool remoteResolved = false;
					{
						std::lock_guard<std::mutex> rl(State::RemoteResolvedMutex);
						remoteResolved = State::RemoteResolved.contains(uuidStr);
					}
					// Normal flow: crop the head from the skin the client already
					// has loaded in memory. Skipped once/if a network avatar for
					// this player has already resolved, so it doesn't get
					// clobbered back to the local crop on a later packet.
					if(!remoteResolved){
						HeadPixels head{};
						if(Head::extractFromEntry(entry, head)){
							std::lock_guard<std::mutex> hl(State::PendingHeadsMutex);
							State::PendingHeads[uuidStr] = std::move(head);
						}
					}

					// Kick off (or no-op if already in flight/resolved) the
					// network check. If the avatar endpoint doesn't respond
					// with a png, this is a pure no-op and the local head
					// above stays as-is.
					auto* xuidStr = reinterpret_cast<const std::string*>(entry + Game::PLP_EntryXuid);
					if(xuidStr && !xuidStr->empty() && xuidStr->size() <= 32){
						Net::AvatarFetcher::instance().request(uuidStr, *xuidStr);
					}

					upsert(uuid, uuidStr, *nameStr);
				}
			}else if(action == 1){
				auto uuidStr = uuidToString(uuid);
				Net::AvatarFetcher::instance().forget(uuidStr);
				{
					std::lock_guard<std::mutex> rl(State::RemoteResolvedMutex);
					State::RemoteResolved.erase(uuidStr);
				}
				m_cache.erase(std::remove_if(m_cache.begin(), m_cache.end(),
					[&](const CachedPlayer& p){ return p.uuid == uuid; }),
					m_cache.end());
			}
		}
		publishPlayers();
	}

	void onSwapBuffers(EGLDisplay dpy, EGLSurface surface){
		if(!m_contextReady){ return; }
		EGLint w = 0, h = 0;
		eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
		eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
		Head::processUploads();
		if(++m_evictCounter % 300 == 0){ Head::evictStale(); }
		if(w > 0 && h > 0 && State::TabHeld.load(std::memory_order_relaxed)){
			if(State::monotonicNow() - State::LastHudTime.load(std::memory_order_relaxed) < 2.0){
				std::vector<PlayerInfo> players;
				{
					std::lock_guard<std::mutex> lock(State::PlayersMutex);
					players = State::Players;
				}
				if(!players.empty()){
					UI::beginFrame((float)w, (float)h);
					UI::drawList(players, (float)w, (float)h);
					UI::endFrame();
				}
			}
		}
	}

	void onMakeCurrent(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx){
		if(!m_contextReady && draw != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT){
			m_contextReady = true;
			UI::init();
		}
	}

	bool onKey(int keyCode, int action){
		if(keyCode == 9 || keyCode == 15){
			if(action == 0){
				State::TabHeld.store(true, std::memory_order_relaxed);
			}else if(action == 2){
				State::TabHeld.store(false, std::memory_order_relaxed);
			}
		}
		return false;
	}

private:
	PlayerBoard() = default;

	void installHooks(){
		struct Ranges { void* r1Start; size_t r1Len; void* r2Start; size_t r2Len; };
		Ranges ranges{};
		dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
			auto* r = static_cast<Ranges*>(data);
			if(!info->dlpi_name){ return 0; }
			if(!std::string(info->dlpi_name).contains("libminecraftpe.so")){ return 0; }
			r->r1Start = reinterpret_cast<void*>(info->dlpi_addr + info->dlpi_phdr[1].p_vaddr);
			r->r1Len   = info->dlpi_phdr[1].p_memsz;
			r->r2Start = reinterpret_cast<void*>(info->dlpi_addr + info->dlpi_phdr[2].p_vaddr);
			r->r2Len   = info->dlpi_phdr[2].p_memsz;
			return 1;
		}, &ranges);
		if(!ranges.r1Start || !ranges.r2Start){
			printf("[headfetch] couldn't find libminecraftpe.so ranges\n");
			return;
		}
		tryHook(ranges.r1Start, ranges.r1Len, ranges.r2Start, ranges.r2Len,
			"17HudCursorRenderer", Game::HudCursor_RenderSlot,
			(void*)hudHook, s_hudOrig, "hud");
		tryHook(ranges.r1Start, ranges.r1Len, ranges.r2Start, ranges.r2Len,
			"16PlayerListPacket", Game::PlayerListPacket_ReadSlot,
			(void*)packetHook, s_pktOrig, "player-list");
	}

	void tryHook(void* r1Start, size_t r1Len, void* r2Start, size_t r2Len,
		const char* name, std::size_t slot, void* hookFn, void*& origOut, const char* label){
		auto occurrences = Scan::findAllInRange(r1Start, r1Len, name);
		for(auto nameAddr : occurrences){
			auto typeinfoRef = Scan::findPtrInRange(r2Start, r2Len, nameAddr);
			if(!typeinfoRef){ continue; }
			auto typeinfo = typeinfoRef - sizeof(void*);
			auto vtableRef = Scan::findPtrInRange(r2Start, r2Len, typeinfo);
			if(!vtableRef){ continue; }
			auto vtable = vtableRef + sizeof(void*);
			auto slotPtr = reinterpret_cast<void**>(vtable + slot * sizeof(void*));
			if(patchSlot(slotPtr, hookFn, origOut)){
				printf("[headfetch] %s hook installed\n", label);
				return;
			}
		}
		printf("[headfetch] %s: typeinfo ref not found\n", label);
	}

	bool patchSlot(void** slot, void* hookFn, void*& origOut){
		long ps = sysconf(_SC_PAGESIZE);
		if(ps <= 0){ return false; }
		auto page = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(slot) & ~(ps - 1));
		if(mprotect(page, ps * 2, PROT_READ | PROT_WRITE | PROT_EXEC) != 0){ return false; }
		origOut = *slot;
		*slot = hookFn;
		// mprotect(page, ps * 2, PROT_READ); //rwx keep shouldn't be necessary to skip either
		return true;
	}

	void resolveEGL(){
		if(s_swapOrig && s_makeCurrentOrig){ return; }
		auto egl = dlopen("libEGL.so", 0);
		if(!egl){ return; }
		if(!s_swapOrig){ s_swapOrig = dlsym(egl, "eglSwapBuffers"); }
		if(!s_makeCurrentOrig){ s_makeCurrentOrig = dlsym(egl, "eglMakeCurrent"); }
		dlclose(egl);
	}

	void installKeyboard(){
		auto gw = dlopen("libmcpelauncher_gamewindow.so", 0);
		if(!gw){ return; }
		using GetWin = void* (*)();
		using AddWCC = void(*)(void*, void(*)(void*));
		using AddKB  = void(*)(void*, void*, bool(*)(void*, int, int));
		auto getWin = (GetWin)dlsym(gw, "game_window_get_primary_window");
		auto addWCC = (AddWCC)dlsym(gw, "game_window_add_window_creation_callback");
		m_addKB = (AddKB)dlsym(gw, "game_window_add_keyboard_callback");
		if(addWCC && getWin && m_addKB){
			m_getWin = getWin;
			addWCC(nullptr, [](void*){
				auto& self = instance();
				if(self.m_getWin){
					auto win = self.m_getWin();
					if(win && self.m_addKB){
						self.m_addKB(win, nullptr, keyboardCb);
					}
				}
			});
		}
	}

	static std::string uuidToString(const UUIDRaw& uuid){
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%016llx%016llx",
			(unsigned long long)uuid.a, (unsigned long long)uuid.b);
		return buf;
	}

	void upsert(const UUIDRaw& uuid, const std::string& uuidStr, const std::string& name){
		for(auto& p : m_cache){
			if(p.uuid == uuid){ p.name = name; return; }
		}
		m_cache.push_back({uuid, uuidStr, name});
	}

	void publishPlayers(){
		State::Players.clear();
		for(const auto& p : m_cache){
			if(!p.name.empty() && p.name.size() <= 64){
				State::Players.push_back({p.name, p.uuidString});
			}
		}
	}

	static void hudHook(void* self, void* ctx, void* client, void* owner, int pass){
		instance().onHudRender(ctx, client);
		if(s_hudOrig){
			using Fn = void(*)(void*, void*, void*, void*, int);
			((Fn)s_hudOrig)(self, ctx, client, owner, pass);
		}
	}

	//declare the real by value return type and let the compiler emit the correct register on each instr set aarch.
	struct alignas(8) PacketReadResult { unsigned char _[80]; };

	static PacketReadResult packetHook(void* self, void* stream){
		PacketReadResult r{};
		if(s_pktOrig){
			using Fn = PacketReadResult(*)(void*, void*);
			r = ((Fn)s_pktOrig)(self, stream);
		}
		instance().onPacketRead(self);
		return r;
	}

	static EGLBoolean eglSwapBuffers_(EGLDisplay dpy, EGLSurface surface){
		instance().onSwapBuffers(dpy, surface);
		if(s_swapOrig){ return ((decltype(&eglSwapBuffers))s_swapOrig)(dpy, surface); }
		return EGL_FALSE;
	}

	static EGLBoolean eglMakeCurrent_(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx){
		instance().onMakeCurrent(dpy, draw, read, ctx);
		if(s_makeCurrentOrig){
			return ((decltype(&eglMakeCurrent))s_makeCurrentOrig)(dpy, draw, read, ctx);
		}
		return EGL_FALSE;
	}

	static bool keyboardCb(void*, int keyCode, int action){
		return instance().onKey(keyCode, action);
	}

	static inline void* s_swapOrig = nullptr;
	static inline void* s_makeCurrentOrig = nullptr;
	static inline void* s_hudOrig = nullptr;
	static inline void* s_pktOrig = nullptr;
	bool  m_contextReady  = false;
	int   m_evictCounter  = 0;
	using GetWin = void* (*)();
	using AddKB  = void(*)(void*, void*, bool(*)(void*, int, int));
	GetWin m_getWin = nullptr;
	AddKB  m_addKB  = nullptr;
	std::vector<CachedPlayer> m_cache;
};

} // namespace hf
