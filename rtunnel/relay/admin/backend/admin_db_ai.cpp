// 账号级 AI 模型配置：每个账号一份（控制台账号 u-<id>、接入账号 c-<id>）。
//
// 密钥本体不在这里：写 <admin.db 同目录>/ai/<subject>.key（0600），
// 本表只存指纹（前 3 + 后 4 位）和时间，避免备份库/看设置的人拿到明文。
#include "admin_db.hpp"

#include "cpputil/json.hpp"

#include <mutex>
#include <sqlite3.h>

using nlohmann::json;

namespace {
const char* kColumns =
    "provider,endpoint,model,key_hint,key_updated_ms,advanced,updated_ms";
}  // namespace

bool AdminDb::aiProfileGet(const std::string& subject, std::string& json_out) {
    json j = json::object();
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    std::string sql = std::string("SELECT ") + kColumns + " FROM ai_profiles WHERE subject=?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        j["provider"] = dbColText(st, 0);
        j["endpoint"] = dbColText(st, 1);
        j["model"] = dbColText(st, 2);
        j["key_hint"] = dbColText(st, 3);
        j["key_updated_ms"] = dbColInt(st, 4);
        std::string adv = dbColText(st, 5);
        if (!adv.empty()) {
            auto a = json::parse(adv, nullptr, false);
            if (!a.is_discarded() && a.is_object()) j["advanced"] = a;
        }
        j["updated_ms"] = dbColInt(st, 6);
        j["saved"] = true;
    } else {
        j["saved"] = false;
    }
    sqlite3_finalize(st);
    json_out = j.dump();
    return true;
}

bool AdminDb::aiProfileSave(const std::string& subject, const std::string& provider,
                            const std::string& endpoint, const std::string& model,
                            const std::string& advanced, std::string& err) {
    if (subject.empty()) {
        err = "缺少主体";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "INSERT INTO ai_profiles(subject,provider,endpoint,model,advanced,updated_ms)"
        " VALUES(?,?,?,?,?,?)"
        " ON CONFLICT(subject) DO UPDATE SET provider=excluded.provider,"
        " endpoint=excluded.endpoint,model=excluded.model,advanced=excluded.advanced,"
        " updated_ms=excluded.updated_ms";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, provider.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, endpoint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, model.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, advanced.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, AdminDb::nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::aiProfileSetKey(const std::string& subject, const std::string& hint,
                              long long updated_ms, std::string& err) {
    if (subject.empty()) {
        err = "缺少主体";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    // 没有行时先建一行空配置，密钥指纹不能丢
    const char* sql =
        "INSERT INTO ai_profiles(subject,key_hint,key_updated_ms,updated_ms) VALUES(?,?,?,?)"
        " ON CONFLICT(subject) DO UPDATE SET key_hint=excluded.key_hint,"
        " key_updated_ms=excluded.key_updated_ms,updated_ms=excluded.updated_ms";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, hint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, updated_ms);
    sqlite3_bind_int64(st, 4, AdminDb::nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    return ok;
}

// ---------------------------------------------------------------- AI 会话记录
//
// 永久保留：这里只有增删改查，没有清理/过期。删除只由用户主动发起。
// 列表默认取最近 N 个（updated_ms 倒序），更早的用 before_ms 翻页。

namespace {

// 取第一行、压掉连续空白，按“字符”截断（中文一个字 3 字节，按字节截会切坏）
std::string autoTitle(const std::string& text, size_t max_chars) {
    std::string t;
    size_t i = 0, chars = 0;
    while (i < text.size()) {
        unsigned char u = (unsigned char)text[i];
        size_t n = (u < 0x80) ? 1 : ((u & 0xE0) == 0xC0) ? 2 : ((u & 0xF0) == 0xE0) ? 3 : 4;
        if (i + n > text.size()) break;
        std::string ch = text.substr(i, n);
        if (ch == "\n" || ch == "\r") break;
        if (ch == "\t") ch = " ";
        if (ch == " " && (t.empty() || t.back() == ' ')) {
            i += n;
            continue;
        }
        if (chars >= max_chars) {
            t += "…";
            break;
        }
        t += ch;
        ++chars;
        i += n;
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t;
}

}  // namespace

bool AdminDb::aiSessionCreate(const std::string& subject, const std::string& title,
                              long long& out_id, std::string& err) {
    if (subject.empty()) {
        err = "缺少主体";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO ai_sessions(subject,title,msg_count,created_ms,updated_ms)"
            " VALUES(?,?,0,?,?)", -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    long long now = AdminDb::nowMs();
    std::string t = title.size() > 80 ? title.substr(0, 80) : title;
    sqlite3_bind_text(st, 1, subject.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, t.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, now);
    sqlite3_bind_int64(st, 4, now);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    if (!ok) err = sqlite3_errmsg(db_);
    sqlite3_finalize(st);
    if (ok) out_id = (long long)sqlite3_last_insert_rowid(db_);
    return ok;
}

bool AdminDb::aiSessionAppend(long long session_id, const std::string& subject,
                              const std::string& role, const std::string& content,
                              const std::string& extra, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    // 用 subject 做条件，避免拼错 id 写到别人的会话里
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO ai_messages(session_id,role,content,extra,created_ms)"
            " SELECT id,?,?,?,? FROM ai_sessions WHERE id=? AND subject=?",
            -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, content.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, extra.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, AdminDb::nowMs());
    sqlite3_bind_int64(st, 5, session_id);
    sqlite3_bind_text(st, 6, subject.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    if (!ok) err = "会话不存在";
    sqlite3_finalize(st);
    if (!ok) return false;

    st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE ai_sessions SET msg_count=msg_count+1,updated_ms=? WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK)
        return true;   // 消息已写入，计数没更新不影响读取
    sqlite3_bind_int64(st, 1, AdminDb::nowMs());
    sqlite3_bind_int64(st, 2, session_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::aiSessionAddTurn(long long session_id, const std::string& subject,
                               const std::string& user_content, const std::string& reply,
                               const std::string& extra, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    // 先确认归属，不然后面两条 INSERT 都得带子查询
    {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT title FROM ai_sessions WHERE id=? AND subject=?",
                               -1, &q, nullptr) != SQLITE_OK) {
            err = "db error";
            return false;
        }
        sqlite3_bind_int64(q, 1, session_id);
        sqlite3_bind_text(q, 2, subject.c_str(), -1, SQLITE_TRANSIENT);
        bool found = sqlite3_step(q) == SQLITE_ROW;
        std::string title = found ? dbColText(q, 0) : std::string();
        sqlite3_finalize(q);
        if (!found) {
            err = "会话不存在";
            return false;
        }
        // 第一轮问答时用问句做标题，后面不再改（除非用户手动重命名）
        if (title.empty()) {
            std::string t = autoTitle(user_content, 30);
            sqlite3_stmt* u = nullptr;
            if (sqlite3_prepare_v2(db_, "UPDATE ai_sessions SET title=? WHERE id=? AND title=''",
                                   -1, &u, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(u, 1, t.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(u, 2, session_id);
                sqlite3_step(u);
                sqlite3_finalize(u);
            }
        }
    }

    if (!exec("BEGIN IMMEDIATE", err)) return false;
    auto fail = [this, &err](const char* msg) {
        std::string e2;
        exec("ROLLBACK", e2);
        err = msg;
        return false;
    };

    long long now = AdminDb::nowMs();
    for (int k = 0; k < 2; ++k) {
        const char* role = (k == 0) ? "user" : "assistant";
        const std::string& body = (k == 0) ? user_content : reply;
        const std::string& ex = (k == 0) ? std::string() : extra;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO ai_messages(session_id,role,content,extra,created_ms)"
                " VALUES(?,?,?,?,?)", -1, &st, nullptr) != SQLITE_OK)
            return fail("db error");
        sqlite3_bind_int64(st, 1, session_id);
        sqlite3_bind_text(st, 2, role, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, body.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, ex.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, now);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) return fail("db error");
    }

    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "UPDATE ai_sessions SET msg_count=msg_count+2,updated_ms=? WHERE id=?",
                -1, &st, nullptr) != SQLITE_OK)
            return fail("db error");
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_int64(st, 2, session_id);
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) return fail("db error");
    }
    return exec("COMMIT", err);
}

bool AdminDb::aiSessionList(const std::string& subject, int limit, long long before_ms,
                            long long before_id, std::string& json_out) {
    if (limit < 1) limit = 10;
    if (limit > 200) limit = 200;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql =
        "SELECT id,title,msg_count,created_ms,updated_ms FROM ai_sessions WHERE subject=?";
    // 复合游标：同一毫秒建的两个会话不会互相盖掉
    if (before_ms > 0) sql += " AND (updated_ms<? OR (updated_ms=? AND id<?))";
    sql += " ORDER BY updated_ms DESC,id DESC LIMIT ?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    sqlite3_bind_text(st, idx++, subject.c_str(), -1, SQLITE_TRANSIENT);
    if (before_ms > 0) {
        sqlite3_bind_int64(st, idx++, before_ms);
        sqlite3_bind_int64(st, idx++, before_ms);
        sqlite3_bind_int64(st, idx++, before_id);
    }
    // 多取一条用来判断还有没有
    sqlite3_bind_int(st, idx++, limit + 1);
    json arr = json::array();
    long long last_ms = 0, last_id = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["id"] = dbColInt(st, 0);
        j["title"] = dbColText(st, 1);
        j["msg_count"] = (int)dbColInt(st, 2);
        j["created_ms"] = dbColInt(st, 3);
        j["updated_ms"] = dbColInt(st, 4);
        last_ms = j["updated_ms"].get<long long>();
        last_id = j["id"].get<long long>();
        arr.push_back(j);
    }
    sqlite3_finalize(st);

    bool has_more = (int)arr.size() > limit;
    while ((int)arr.size() > limit) arr.erase(arr.size() - 1);
    // 游标必须是“本页最后一条”，不是多取的那条，否则翻页会跳过一条
    if (!arr.empty()) {
        last_ms = arr.back().value("updated_ms", (long long)0);
        last_id = arr.back().value("id", (long long)0);
    }
    json out;
    out["items"] = arr;
    out["has_more"] = has_more;
    out["next_before_ms"] = has_more ? last_ms : 0;
    out["next_before_id"] = has_more ? last_id : 0;
    json_out = out.dump();
    return true;
}

bool AdminDb::aiSessionGet(long long id, const std::string& subject, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json out = json::object();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id,title,msg_count,created_ms,updated_ms FROM ai_sessions"
            " WHERE id=? AND subject=?", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, subject.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;   // 不存在或不是自己的
    }
    out["id"] = dbColInt(st, 0);
    out["title"] = dbColText(st, 1);
    out["msg_count"] = (int)dbColInt(st, 2);
    out["created_ms"] = dbColInt(st, 3);
    out["updated_ms"] = dbColInt(st, 4);
    sqlite3_finalize(st);

    json msgs = json::array();
    st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT role,content,extra,created_ms FROM ai_messages WHERE session_id=?"
            " ORDER BY id", -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        json m;
        m["role"] = dbColText(st, 0);
        m["content"] = dbColText(st, 1);
        std::string extra = dbColText(st, 2);
        if (!extra.empty()) {
            auto e = json::parse(extra, nullptr, false);
            if (!e.is_discarded() && e.is_object()) {
                for (auto it = e.begin(); it != e.end(); ++it) m[it.key()] = it.value();
            }
        }
        m["created_ms"] = dbColInt(st, 3);
        msgs.push_back(m);
    }
    sqlite3_finalize(st);
    out["messages"] = msgs;
    json_out = out.dump();
    return true;
}

bool AdminDb::aiSessionRename(long long id, const std::string& subject, const std::string& title,
                              std::string& err) {
    std::string t = title;
    if (t.empty()) {
        err = "标题不能为空";
        return false;
    }
    if (t.size() > 80) t = t.substr(0, 80);
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE ai_sessions SET title=?,updated_ms=? WHERE id=? AND subject=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, t.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, AdminDb::nowMs());
    sqlite3_bind_int64(st, 3, id);
    sqlite3_bind_text(st, 4, subject.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        err = "会话不存在";
        return false;
    }
    return true;
}

bool AdminDb::aiSessionDelete(long long id, const std::string& subject, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM ai_sessions WHERE id=? AND subject=?", -1, &st,
                           nullptr) != SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_bind_text(st, 2, subject.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    if (sqlite3_changes(db_) == 0) {
        err = "会话不存在";
        return false;
    }
    st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM ai_messages WHERE session_id=?", -1, &st, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
    return true;
}
