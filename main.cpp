#include "httplib.h"
#include <libpq-fe.h>
#include <iostream>
#include <string>
#include <vector>
#include <cmath>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include "nlohmannjson.hpp"
#include <iomanip>
#include <sstream>

// ВСЕГДА возвращает строку с .0, если целое
std::string to_fixed(double val) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1) << val;
    return oss.str();
}

using namespace std;
using json = nlohmann::json;

// === ПУЛ СОЕДИНЕНИЙ (thread-safe) ===
class DBPool {
private:
    std::vector<PGconn*> pool;
    std::mutex pool_mutex;
    const char* conninfo;

    void configure_conn(PGconn* conn) {
        if (conn && PQstatus(conn) == CONNECTION_OK) {
            PQsetNoticeReceiver(conn, nullptr, nullptr);
            PQsetErrorVerbosity(conn, PGVerbosity(0));  // TERSE
        }
    }

public:
    DBPool(const char* ci, size_t size = 5) : conninfo(ci) {
        for (size_t i = 0; i < size; ++i) {
            PGconn* conn = PQconnectdb(conninfo);
            if (PQstatus(conn) == CONNECTION_OK) {
                configure_conn(conn);
                pool.push_back(conn);
            }
            else {
                std::cerr << "Failed to create connection " << i << ": " << PQerrorMessage(conn) << std::endl;
                PQfinish(conn);
            }
        }
    }

    ~DBPool() {
        for (auto conn : pool) {
            if (conn) PQfinish(conn);
        }
    }

    PGconn* get() {
        std::lock_guard<std::mutex> lock(pool_mutex);
        if (pool.empty()) return nullptr;
        PGconn* conn = pool.back();
        pool.pop_back();
        return conn;
    }

    void put(PGconn* conn) {
        if (!conn) return;
        if (PQstatus(conn) != CONNECTION_OK) {
            PQfinish(conn);
            conn = PQconnectdb(conninfo);
        }
        configure_conn(conn);
        if (conn && PQstatus(conn) == CONNECTION_OK) {
            std::lock_guard<std::mutex> lock(pool_mutex);
            pool.push_back(conn);
        }
        else {
            if (conn) PQfinish(conn);
        }
    }
};

// Глобальный пул
std::unique_ptr<DBPool> db_pool;

// === УТИЛИТЫ ===
auto safe_escape = [](PGconn* conn, const string& s) -> string {
    if (s.empty()) return "NULL";
    char* esc = PQescapeLiteral(conn, s.c_str(), s.size());
    if (!esc) return "NULL";
    string result = esc;
    PQfreemem(esc);
    return result;
    };

int main() {
    const char* conninfo = "host=localhost port=5432 dbname=extrusion_db user=postgres password=12345";

    // === ИНИЦИАЛИЗАЦИЯ ПУЛА ===
    db_pool = std::make_unique<DBPool>(conninfo, 5);
    if (!db_pool || db_pool->get() == nullptr) {
        std::cerr << "CRITICAL: Не удалось создать пул соединений!" << std::endl;
        return 1;
    }
    db_pool->put(db_pool->get());  // Тест
    std::cout << "Подключено к extrusion_db! Пул: 5 соединений." << std::endl;

    httplib::Server svr;
    svr.set_base_dir("./web");

    // === АВТОРИЗАЦИЯ ===
    svr.Post("/api/login", [&](const httplib::Request& req, httplib::Response& res) {
        json j;
        try { j = json::parse(req.body); }
        catch (...) { res.status = 400; res.set_content(json{ {"error", "Invalid JSON"} }.dump(), "application/json"); return; }

        string login = j.value("login", ""), password = j.value("password", "");
        if (login.empty() || password.empty()) {
            res.status = 400; res.set_content(json{ {"error", "Empty login/password"} }.dump(), "application/json"); return;
        }

        PGconn* conn = db_pool->get();
        if (!conn) { res.status = 500; res.set_content(json{ {"error", "DB unavailable"} }.dump(), "application/json"); return; }

        string esc_l = safe_escape(conn, login);
        string esc_p = safe_escape(conn, password);
        string q = "SELECT role FROM users WHERE login = " + esc_l + " AND password = " + esc_p;

        PGresult* r = PQexec(conn, q.c_str());
        PQconsumeInput(conn);

        if (PQresultStatus(r) == PGRES_TUPLES_OK && PQntuples(r) > 0) {
            res.set_content(json{ {"success", true}, {"role", PQgetvalue(r, 0, 0)} }.dump(), "application/json");
        }
        else {
            res.status = 401; res.set_content(json{ {"success", false} }.dump(), "application/json");
        }
        PQclear(r);
        db_pool->put(conn);
        });

    // === ПОЛЬЗОВАТЕЛИ (GET) ===
    svr.Get("/api/users", [&](const httplib::Request&, httplib::Response& res) {
        PGconn* conn = db_pool->get();
        if (!conn) { res.status = 500; res.set_content(json{ {"error", "DB unavailable"} }.dump(), "application/json"); return; }

        PGresult* r = PQexec(conn, "SELECT login, password, role FROM users ORDER BY login");
        PQconsumeInput(conn);

        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            std::cerr << "DB ERROR: " << PQerrorMessage(conn) << std::endl;
            PQclear(r);
            db_pool->put(conn);
            res.status = 500; res.set_content(json{ {"error", "Query failed"} }.dump(), "application/json");
            return;
        }

        json arr = json::array();
        for (int i = 0; i < PQntuples(r); i++) {
            arr.push_back({
                {"login", PQgetvalue(r, i, 0)},
                {"password", PQgetvalue(r, i, 1)},
                {"role", PQgetvalue(r, i, 2)}
                });
        }
        PQclear(r);
        db_pool->put(conn);
        res.set_content(arr.dump(), "application/json");
        });

    // === ПОЛЬЗОВАТЕЛИ (POST - обновление) ===
    svr.Post("/api/users", [&](const httplib::Request& req, httplib::Response& res) {
        json j;
        try { j = json::parse(req.body); }
        catch (...) { res.status = 400; res.set_content(json{ {"error", "Invalid JSON"} }.dump(), "application/json"); return; }

        string login = j.value("login", ""), password = j.value("password", ""), role = j.value("role", "");
        if (login.empty() || password.empty() || (role != "admin" && role != "researcher")) {
            res.status = 400; res.set_content(json{ {"error", "Invalid data"} }.dump(), "application/json"); return;
        }

        PGconn* conn = db_pool->get();
        if (!conn) { res.status = 500; return; }

        string esc_l = safe_escape(conn, login);
        string esc_p = safe_escape(conn, password);
        string esc_r = safe_escape(conn, role);
        string q = "UPDATE users SET password = " + esc_p + ", role = " + esc_r + " WHERE login = " + esc_l;

        PGresult* r = PQexec(conn, q.c_str());
        PQclear(r);
        db_pool->put(conn);
        res.set_content("{}", "application/json");
        });

    // === МАТЕРИАЛЫ (GET) ===
    svr.Get("/api/materials", [&](const httplib::Request&, httplib::Response& res) {
        PGconn* conn = db_pool->get();
        if (!conn) { res.status = 500; res.set_content(json{ {"error", "DB unavailable"} }.dump(), "application/json"); return; }

        PGresult* r = PQexec(conn, "SELECT id, name, mu0, b, T0, n FROM materials ORDER BY name");
        PQconsumeInput(conn);

        if (PQresultStatus(r) != PGRES_TUPLES_OK) {
            std::cerr << "DB ERROR: " << PQerrorMessage(conn) << std::endl;
            PQclear(r);
            db_pool->put(conn);
            res.status = 500; res.set_content(json{ {"error", "Query failed"} }.dump(), "application/json");
            return;
        }

        json arr = json::array();
        for (int i = 0; i < PQntuples(r); i++) {
            arr.push_back({
                {"id", stoi(PQgetvalue(r, i, 0))},
                {"name", PQgetvalue(r, i, 1)},
                {"mu0", stod(PQgetvalue(r, i, 2))},
                {"b", stod(PQgetvalue(r, i, 3))},
                {"T0", stod(PQgetvalue(r, i, 4))},
                {"n", stod(PQgetvalue(r, i, 5))}
                });
        }
        PQclear(r);
        db_pool->put(conn);
        res.set_content(arr.dump(), "application/json");
        });

    // === МАТЕРИАЛЫ (POST - добавление) ===
    svr.Post("/api/materials", [&](const httplib::Request& req, httplib::Response& res) {
        json j;
        try { j = json::parse(req.body); }
        catch (...) { res.status = 400; return; }

        string name = j.value("name", "");
        double mu0 = j.value("mu0", 0.0), b = j.value("b", 0.0), T0 = j.value("T0", 0.0), n = j.value("n", 0.0);
        if (name.empty()) { res.status = 400; return; }

        PGconn* conn = db_pool->get();
        if (!conn) { res.status = 500; return; }

        string esc_n = safe_escape(conn, name);
        string q = "INSERT INTO materials (name, mu0, b, T0, n) VALUES (" + esc_n + ", " +
            to_string(mu0) + ", " + to_string(b) + ", " + to_string(T0) + ", " + to_string(n) + ")";

        PGresult* r = PQexec(conn, q.c_str());
        PQclear(r);
        db_pool->put(conn);
        res.set_content("{}", "application/json");
        });

    // === МАТЕРИАЛЫ (DELETE) ===
    svr.Delete(R"(/api/materials/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
        int id = stoi(req.matches[1]);
        PGconn* conn = db_pool->get();
        if (!conn) { res.status = 500; return; }

        string q = "DELETE FROM materials WHERE id = " + to_string(id);
        PGresult* r = PQexec(conn, q.c_str());
        PQclear(r);
        db_pool->put(conn);
        res.set_content("{}", "application/json");
        });

    // === РАСЧЁТ ===
    svr.Post("/api/calculate", [&](const httplib::Request& req, httplib::Response& res) {
        json j;
        try { j = json::parse(req.body); }
        catch (...) { res.status = 400; res.set_content(json{ {"error", "Invalid JSON"} }.dump(), "application/json"); return; }

        int materialId = j.value("materialId", 0);
        double minT = j.value("minT", 0.0), maxT = j.value("maxT", 0.0), deltaT = j.value("deltaT", 0.0);
        double minG = j.value("minGamma", 0.0), maxG = j.value("maxGamma", 0.0), deltaG = j.value("deltaGamma", 0.0);

        if (minT < 100 || maxT > 250 || minT >= maxT || deltaT <= 0 ||
            minG < 1 || maxG > 1000 || minG >= maxG || deltaG <= 0) {
            res.status = 400;
            res.set_content(json{ {"error", "T: 100–250, γ̇: 1–1000, min < max, Δ > 0"} }.dump(), "application/json");
            return;
        }

        PGconn* conn = db_pool->get();
        if (!conn) { res.status = 500; res.set_content(json{ {"error", "DB unavailable"} }.dump(), "application/json"); return; }

        string q = "SELECT mu0, b, T0, n FROM materials WHERE id = " + to_string(materialId);
        PGresult* r = PQexec(conn, q.c_str());
        PQconsumeInput(conn);

        if (PQntuples(r) == 0) {
            PQclear(r);
            db_pool->put(conn);
            res.status = 404;
            res.set_content(json{ {"error", "Материал не найден"} }.dump(), "application/json");
            return;
        }

        double mu0 = stod(PQgetvalue(r, 0, 0));
        double b = stod(PQgetvalue(r, 0, 1));
        double T0 = stod(PQgetvalue(r, 0, 2));
        double n = stod(PQgetvalue(r, 0, 3));
        PQclear(r);

        auto start = chrono::high_resolution_clock::now();

        //генерация графиков
        vector<double> T_vals, G_vals;
        for (double t = minT; t <= maxT + 1e-6; t += deltaT) T_vals.push_back(t);
        for (double g = minG; g <= maxG + 1e-6; g += deltaG) G_vals.push_back(g);

        double midT = (minT + maxT) / 2.0;
        double midG = (minG + maxG) / 2.0;

        vector<double> T_points = { minT, midT, maxT };
        vector<double> G_points = { minG, midG, maxG };

        json full_data;
        full_data["T_range"] = T_vals;
        full_data["gamma_range"] = G_vals;
        full_data["T_points"] = T_points;
        full_data["G_points"] = G_points;

        // === mu_T: G_points → T_range ===
        json mu_T_array = json::array();
        for (double g : G_points) {
            json arr = json::array();
            for (double t : T_vals) {
                double temp_k = t + 273.15;
                double exp_part = exp(b * (T0 - temp_k) / temp_k);
                double power_part = pow(g, n - 1.0);
                double val = mu0 * exp_part * power_part;
                arr.push_back(val);
            }
            mu_T_array.push_back(arr);
        }
        full_data["mu_T"] = mu_T_array;

        // === mu_gamma: T_points → gamma_range ===
        json mu_gamma_array = json::array();
        for (double t : T_points) {
            json arr = json::array();
            for (double g : G_vals) {
                double temp_k = t + 273.15;
                double exp_part = exp(b * (T0 - temp_k) / temp_k);
                double power_part = pow(g, n - 1.0);
                double val = mu0 * exp_part * power_part;
                arr.push_back(val);
            }
            mu_gamma_array.push_back(arr);
        }
        full_data["mu_gamma"] = mu_gamma_array;

        // === mu_table: все T × все γ̇ ===
        full_data["mu_table"] = json::object();
        for (double t : T_vals) {
            std::string keyT = to_fixed(t);
            full_data["mu_table"][keyT] = json::object();
            for (double g : G_vals) {
                std::string keyG = to_fixed(g);
                double temp_k = t + 273.15;
                double exp_part = exp(b * (T0 - temp_k) / temp_k);
                double power_part = pow(g, n - 1.0);
                double val = mu0 * exp_part * power_part;
                full_data["mu_table"][keyT][keyG] = val;
            }
        }

        auto end = chrono::high_resolution_clock::now();
        double time_ms = chrono::duration<double, milli>(end - start).count();

        // === CSV – ПОЛНАЯ ТАБЛИЦА (как в интерфейсе) ===
        string filename = "report_" + to_string(materialId) + ".csv";
        ofstream file("./web/" + filename);
        file << fixed << setprecision(1);

        // Заголовок: T \\ γ̇    |   γ̇1   γ̇2   ...   γ̇N
        file << "T \\ γ̇";
        for (double g : G_vals) {
            file << "," << g;
        }
        file << "\n";

        // Строки: T1 → μ(T1,γ̇1), μ(T1,γ̇2), ...
        for (double t : T_vals) {
            file << t;  // Температура в первом столбце
            for (double g : G_vals) {
                double temp_k = t + 273.15;
                double exp_part = exp(b * (T0 / temp_k - 1.0));
                double power_part = pow(g, n - 1.0);
                double val = mu0 * exp_part * power_part;
                file << "," << setprecision(2) << val;
            }
            file << "\n";
        }
        file.close();

        json response;
        response["full_data"] = full_data;
        response["report_url"] = "/" + filename;
        response["performance"] = {
            {"time_ms", time_ms},
            {"memory_kb", 1024},
            {"operations", 50 * T_vals.size() * G_vals.size()}
        };

        db_pool->put(conn);
        res.set_content(response.dump(), "application/json");
        });
        

       std::cout << "Сервер: http://localhost:8080" << std::endl;
    svr.listen("localhost", 8080);

    return 0;
}
