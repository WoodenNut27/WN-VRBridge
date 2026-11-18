#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // テスト名と失敗の説明に日本語を使うので、コンソールの出力を UTF-8 にそろえる。
    SetConsoleOutputCP(CP_UTF8);
#endif
    doctest::Context context(argc, argv);
    return context.run();
}
