// csrc/angle_config.cpp

#include "csrc/angle_config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "csrc/log.hpp"

namespace csrc {

namespace {

const char* kAnglesFile = "arm_angles.json";

Json defaults_for(const std::string& driver) {
    Json d;
    if (driver == STS3215_DRIVER) {
        d["grab_position"]["servo1"] = Json((int64_t)1850);
        d["grab_position"]["servo2"] = Json((int64_t)2650);
        d["lift_position"]["servo1"] = Json((int64_t)2300);
        d["lift_position"]["servo2"] = Json((int64_t)2100);
        d["gripper_open"] = Json((int64_t)4000);
        d["gripper_close"] = Json((int64_t)3000);
    } else {  // zp10s
        d["grab_position"]["servo0"] = Json((int64_t)245);
        d["grab_position"]["servo1"] = Json((int64_t)180);
        d["lift_position"]["servo0"] = Json((int64_t)200);
        d["lift_position"]["servo1"] = Json((int64_t)180);
        d["gripper_open"] = Json((int64_t)150);
        d["gripper_close"] = Json((int64_t)90);
    }
    return d;
}

// 从 Json 中安全取 int（非数字 → def）
int64_t as_int_or(const Json& j, int64_t def) {
    if (j.is_number()) return (int64_t)j.as_double();
    if (j.is_string()) return j.as_int(def);
    return def;
}

/// 标准化：用默认值填充缺失字段（对齐 Python _normalize_angles）
Json normalize_angles(const Json& data, const Json& defaults) {
    Json out;
    for (const char* sk : {"gripper_open", "gripper_close"}) {
        const Json* v = data.get(sk);
        int64_t def = as_int_or(defaults.get(sk) ? *defaults.get(sk) : Json(Json::Type::Null), 0);
        out[sk] = Json(v ? as_int_or(*v, def) : def);
    }
    for (const char* gk : {"grab_position", "lift_position"}) {
        const Json* dg = defaults.get(gk);
        const Json* sg = data.get(gk);
        Json group;
        if (dg && dg->is_object()) {
            for (auto& kv : dg->object()) {
                int64_t def = as_int_or(kv.second, 0);
                const Json* sv = sg && sg->is_object() ? sg->get(kv.first) : nullptr;
                group[kv.first] = Json(sv ? as_int_or(*sv, def) : def);
            }
        }
        out[gk] = group;
    }
    return out;
}

bool is_old_format(const Json& data) {
    if (!data.is_object()) return false;
    for (auto& kv : data.object()) {
        const std::string& k = kv.first;
        if (k.rfind("servo", 0) == 0 &&
            (k.find("_prepare") != std::string::npos || k.find("_lift") != std::string::npos ||
             k.find("_grab") != std::string::npos || k.find("_approach") != std::string::npos ||
             k.find("_enter") != std::string::npos)) {
            return true;
        }
    }
    return false;
}

Json migrate_old_format(const Json& data, const std::string& driver) {
    auto g = [&](const char* key, int64_t def) -> int64_t {
        const Json* v = data.get(key);
        return v ? as_int_or(*v, def) : def;
    };
    Json out;
    if (driver == STS3215_DRIVER) {
        out["grab_position"]["servo1"] = Json(g("servo1_enter", 1850));
        out["grab_position"]["servo2"] = Json(g("servo2_enter", 2650));
        out["lift_position"]["servo1"] = Json(g("servo1_lift", 2300));
        out["lift_position"]["servo2"] = Json(g("servo2_lift", 2100));
        out["gripper_open"] = Json(g("servo3_prepare", 4000));
        out["gripper_close"] = Json(g("servo3_grab", 3000));
    } else {
        out["grab_position"]["servo0"] = Json(g("servo0_prepare", 245));
        out["grab_position"]["servo1"] = Json(g("servo1_prepare", 180));
        out["lift_position"]["servo0"] = Json(g("servo0_lift", 200));
        out["lift_position"]["servo1"] = Json(g("servo1_lift", 180));
        out["gripper_open"] = Json(g("servo2_prepare", 150));
        out["gripper_close"] = Json(g("servo2_grab", 90));
    }
    return out;
}

Json read_angles_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return Json(Json::Type::Object);
    std::stringstream ss;
    ss << f.rdbuf();
    Json out;
    if (!Json::parse(ss.str(), out) || !out.is_object()) return Json(Json::Type::Object);
    return out;
}

void write_angles_file(const std::string& path, const Json& data) {
    std::ofstream f(path);
    if (f) {
        f << data.dump(true, 0) << "\n";  // 2 空格缩进，与 Python indent=2 一致
    }
}

bool is_multi_driver(const Json& raw) {
    if (!raw.is_object()) return false;
    for (auto& kv : raw.object()) {
        if (kv.first == ZP10S_DRIVER || kv.first == STS3215_DRIVER) return true;
    }
    return false;
}

}  // namespace

std::string arm_angles_path() {
    // 优先级：ARM_ANGLES_PATH > $AKA_HOME/arm_angles.json > cwd/arm_angles.json
    if (const char* env = std::getenv("ARM_ANGLES_PATH")) return env;
    if (const char* home = std::getenv("AKA_HOME")) {
        std::string p = std::string(home) + "/arm_angles.json";
        std::ifstream f(p);
        if (f.good()) return p;
    }
    return kAnglesFile;
}

Json default_arm_angles(const std::string& driver) {
    return defaults_for(driver);
}

Json load_arm_angles(const std::string& driver) {
    Json defaults = defaults_for(driver);
    Json raw = read_angles_file(arm_angles_path());

    Json source;
    if (raw.get(driver) && raw.get(driver)->is_object()) {
        source = *raw.get(driver);
    } else if (driver == ZP10S_DRIVER) {
        source = raw;
    } else {
        source = Json(Json::Type::Object);
    }

    if (is_old_format(source)) {
        Json migrated = migrate_old_format(source, driver);
        // 写回迁移后的格式
        if (driver == ZP10S_DRIVER && !is_multi_driver(raw)) {
            write_angles_file(arm_angles_path(), migrated);
        } else {
            Json data = is_multi_driver(raw) ? raw : Json(Json::Type::Object);
            data[driver] = migrated;
            write_angles_file(arm_angles_path(), data);
        }
        return normalize_angles(migrated, defaults);
    }
    return normalize_angles(source, defaults);
}

Json save_arm_angles(const std::string& driver, const Json& angles) {
    Json defaults = defaults_for(driver);
    Json normalized = normalize_angles(angles, defaults);
    Json raw = read_angles_file(arm_angles_path());

    bool multi = is_multi_driver(raw);
    if (driver == ZP10S_DRIVER && !multi) {
        write_angles_file(arm_angles_path(), normalized);
    } else {
        Json data = multi ? raw : Json(Json::Type::Object);
        data[driver] = normalized;
        write_angles_file(arm_angles_path(), data);
    }
    return normalized;
}

int get_grab_servo(const Json& angles, const std::string& servo_key, const std::string& driver) {
    const Json* gp = angles.get("grab_position");
    if (gp && gp->is_object()) {
        const Json* v = gp->get(servo_key);
        if (v) return (int)as_int_or(*v, 0);
    }
    const Json* dg = defaults_for(driver).get("grab_position");
    if (dg && dg->is_object()) {
        const Json* v = dg->get(servo_key);
        if (v) return (int)as_int_or(*v, 0);
    }
    return 0;
}

int get_lift_servo(const Json& angles, const std::string& servo_key, const std::string& driver) {
    const Json* lp = angles.get("lift_position");
    if (lp && lp->is_object()) {
        const Json* v = lp->get(servo_key);
        if (v) return (int)as_int_or(*v, 0);
    }
    const Json* dl = defaults_for(driver).get("lift_position");
    if (dl && dl->is_object()) {
        const Json* v = dl->get(servo_key);
        if (v) return (int)as_int_or(*v, 0);
    }
    return 0;
}

int get_gripper_open(const Json& angles, const std::string& driver) {
    const Json* v = angles.get("gripper_open");
    int def = (int)as_int_or(*defaults_for(driver).get("gripper_open"), 0);
    return v ? (int)as_int_or(*v, def) : def;
}

int get_gripper_close(const Json& angles, const std::string& driver) {
    const Json* v = angles.get("gripper_close");
    int def = (int)as_int_or(*defaults_for(driver).get("gripper_close"), 0);
    return v ? (int)as_int_or(*v, def) : def;
}

}  // namespace csrc
