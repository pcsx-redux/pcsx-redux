// #embed (C23, C++26 P1967) in C++. data.bin is 16 bytes, 0x89 first and 0x5a last.
// Everything is checked at compile time, so a compiler that skips the directive
// or embeds the wrong bytes fails to build.

#if !defined(__has_embed)
#error "__has_embed is not defined"
#else
#if !__has_embed("data.bin")
#error "__has_embed(\"data.bin\") is zero"
#endif
#endif

static constexpr unsigned char data[] = {
#embed "data.bin"
};

static_assert(sizeof(data) == 16);
static_assert(data[0] == 0x89);
static_assert(data[sizeof(data) - 1] == 0x5a);

int main() { return data[0] == 0x89 ? 0 : 1; }
