// mcpelauncher's Android-compat shim does not export memrchr, but libcurl
// (built from source) references it expecting to import it from libc at
// runtime. Defining it here means the linker resolves the reference
// statically while building libheadfetch.so, so dlopen never needs to look
// it up dynamically. See: "cannot locate symbol memrchr" mod-load failure.
#include <cstddef>

extern "C" void* memrchr(const void* s, int c, std::size_t n){
	const unsigned char* p = static_cast<const unsigned char*>(s);
	const unsigned char ch = static_cast<unsigned char>(c);
	for(std::size_t i = n; i > 0; --i){
		if(p[i - 1] == ch){
			return const_cast<unsigned char*>(p + i - 1);
		}
	}
	return nullptr;
}
