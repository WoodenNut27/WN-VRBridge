#pragma once

#include <string>
#include <vector>

namespace wnvr::driver::testing {

// テストのあいだにドライバのコードが出したログ。
std::vector<std::string>& LogLines();

}
