#pragma once
// 長さの単位。シーン内部の数値は常にメートルとして扱う。
#include <cmath>
#include <cstdio>
#include <string>

enum LengthUnit { Unit_MM = 0, Unit_CM, Unit_M, Unit_Inch, Unit_Foot, Unit_Count };

inline const char* UnitName(int u) {
    static const char* n[] = { "ミリメートル (mm)", "センチメートル (cm)", "メートル (m)", "インチ (in)", "フィート (ft)" };
    return u >= 0 && u < Unit_Count ? n[u] : "不明";
}
inline const char* UnitSuffix(int u) {
    static const char* s[] = { "mm", "cm", "m", "in", "ft" };
    return u >= 0 && u < Unit_Count ? s[u] : "";
}
inline double UnitToMeters(int u) {
    static const double f[] = { 0.001, 0.01, 1.0, 0.0254, 0.3048 };
    return u >= 0 && u < Unit_Count ? f[u] : 1.0;
}
inline const char* UnitComboItems() { return "ミリメートル (mm)\0センチメートル (cm)\0メートル (m)\0インチ (in)\0フィート (ft)\0"; }

// FBX の UnitScaleFactor (1 単位が何 cm か) から最も近い単位を返す。該当なしは -1
inline int UnitFromFbxScaleFactor(double cmPerUnit) {
    const double cand[] = { 0.1, 1.0, 100.0, 2.54, 30.48 };
    for (int i = 0; i < Unit_Count; ++i)
        if (std::abs(cmPerUnit - cand[i]) < cand[i] * 1e-3) return i;
    return -1;
}

// メートル値を指定単位の文字列にする (桁数は値の大きさに応じて調整)
inline std::string FormatLength(double meters, int unit, bool withSuffix = true) {
    double v = meters / UnitToMeters(unit);
    double a = std::abs(v);
    int digits = a >= 1000 ? 1 : a >= 100 ? 2 : a >= 1 ? 3 : 4;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f%s%s", digits, v, withSuffix ? " " : "", withSuffix ? UnitSuffix(unit) : "");
    return buf;
}
