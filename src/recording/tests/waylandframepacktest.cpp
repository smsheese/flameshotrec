#include "wayland/portalpipesource.h"

#include <cstdio>

int main()
{
    // Two BGRA pixels per row with four bytes of PipeWire row padding.
    const unsigned char source[] = {
      1, 2, 3, 4, 5, 6, 7, 8, 90, 91, 92, 93,
      9, 10, 11, 12, 13, 14, 15, 16, 94, 95, 96, 97 };
    const QByteArray packed = Recording::packBgraFrame(source, sizeof(source), 0, 12, 2, 2);
    const QByteArray expected("\1\2\3\4\5\6\7\10\11\12\13\14\15\16\17\20", 16);
    if (packed != expected) {
        std::fprintf(stderr, "PipeWire row stride was not packed correctly\n");
        return 1;
    }
    if (!Recording::packBgraFrame(source, 15, 0, 12, 2, 2).isEmpty()) {
        std::fprintf(stderr, "Truncated PipeWire frame was accepted\n");
        return 1;
    }
    if (!Recording::packBgraFrame(source, sizeof(source), 0, 4, 2, 2).isEmpty()) {
        std::fprintf(stderr, "Invalid stride was accepted\n");
        return 1;
    }
    return 0;
}
