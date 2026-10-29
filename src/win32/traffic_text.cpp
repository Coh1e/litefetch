#include "traffic_text.h"

#include <cstdio>

namespace litefetch::ui {
namespace {
std::string sizeText(std::int64_t bytes)
{
    if (bytes < 0) bytes = 0;
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    char buffer[64]{};
    if (unit == 0) std::snprintf(buffer, sizeof(buffer), "%lld B", static_cast<long long>(bytes));
    else std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
    return buffer;
}
} // namespace

std::string trafficSummary(const std::array<std::int64_t, 4>& totals)
{
    std::string text = "流量：下载 " + sizeText(totals[0]) + " · 上传 " + sizeText(totals[1])
        + " · 校验 " + sizeText(totals[3]);
    if (totals[2] > 0) text += " · 元数据 " + sizeText(totals[2]);
    else text += "（元数据未统计）";
    return text;
}

} // namespace litefetch::ui
