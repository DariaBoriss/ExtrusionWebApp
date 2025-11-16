# README: Инструкция по запуску программы "Исследование экструзии полимеров"

Эта программа представляет собой веб-приложение для моделирования процесса экструзии полимерных материалов. Она включает сервер на C++ (с использованием httplib, nlohmann/json и PostgreSQL) и веб-интерфейс (HTML, JavaScript, Chart.js). Программа позволяет авторизоваться, администрировать материалы и пользователей, а также проводить расчеты и визуализацию.

## Требования к системе
- **Операционная система**: Windows (рекомендуется 64-bit), Linux или macOS.
- **PostgreSQL**: Версия 10+ (для базы данных). Скачайте с официального сайта: https://www.postgresql.org/download/.
- **C++ компилятор**: g++ (из GCC) или MSVC (Visual Studio). Рекомендуется C++11+.
- **Библиотеки**:
  - httplib.h (в комплекте, но убедитесь, что он в папке с проектом).
  - nlohmann/json.hpp (в комплекте).
  - libpq (библиотека для PostgreSQL, обычно устанавливается с PostgreSQL).
- **Браузер**: Chrome, Firefox или Edge для просмотра веб-интерфейса.
- **Дополнительно**: Убедитесь, что у вас есть права администратора для установки ПО.

## Шаги по установке и запуску

### 1. Установка PostgreSQL
- Скачайте и установите PostgreSQL.
- Во время установки укажите пароль для пользователя `postgres` (по умолчанию в программе: `12345`).
- Создайте базу данных:
  - Откройте pgAdmin (или используйте psql в командной строке).
  - Подключитесь как `postgres`.
  - Выполните SQL-запросы для создания базы и таблиц:

```sql
CREATE DATABASE extrusion_db;

-- Подключитесь к extrusion_db и выполните:
CREATE TABLE materials (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    mu0 DOUBLE PRECISION NOT NULL,
    b DOUBLE PRECISION NOT NULL,
    T0 DOUBLE PRECISION NOT NULL,
    n DOUBLE PRECISION NOT NULL
);

CREATE TABLE users (
    login VARCHAR(50) PRIMARY KEY,
    password VARCHAR(50) NOT NULL,
    role VARCHAR(20) NOT NULL CHECK (role IN ('admin', 'researcher'))
);

-- Вставьте тестовые данные (измените по необходимости):
INSERT INTO users (login, password, role) VALUES ('admin', 'adminpass', 'admin');
INSERT INTO users (login, password, role) VALUES ('researcher', 'respass', 'researcher');

-- Пример материалов (из задания варианта 6):
INSERT INTO materials (name, mu0, b, T0, n) VALUES ('ПВД', 2300, 11500, 190, 0.3);
INSERT INTO materials (name, mu0, b, T0, n) VALUES ('ПНД', 1800, 9800, 200, 0.4);
INSERT INTO materials (name, mu0, b, T0, n) VALUES ('ПП', 1500, 8500, 210, 0.35);

- Убедитесь, что строка подключения в main.cpp соответствует: `host=localhost port=5432 dbname=extrusion_db user=postgres password=12345`. Если пароль другой, измените в коде.

### 2. Подготовка проекта
- Создайте папку проекта и скопируйте в нее файлы:
  - main.cpp
  - httplib.h
  - nlohmannjson.hpp 
  - chart.min.js
  - index.html
  - admin.html
  - researcher.html
  - Создайте подпапку `web` и поместите в нее HTML/JS файлы (index.html, admin.html, researcher.html, chart.min.js).

### 3. Компиляция программы
- Откройте командную строку (cmd в Windows) или терминал.
- Перейдите в папку с main.cpp.
- Скомпилируйте с помощью g++ (если установлен GCC/MinGW):

### 4. Запуск сервера
- Сервер запустится на http://localhost:8080.
- Откройте браузер и перейдите по адресу: http://localhost:8080

### 5. Использование программы
- На главной странице (index.html): Войдите как admin (логин: admin, пароль: adminpass) или researcher.
- Для админа: Перейдет в admin.html для управления материалами и пользователями.
- Для исследователя: В researcher.html выберите материал, укажите параметры (T min/max, ΔT, γ min/max, Δγ) и нажмите "Рассчитать".
- Результаты: Таблица и графики вязкости, возможность скачать отчет в CSV. 

### 6. Возможные проблемы и решения
- Ошибка подключения к БД: Проверьте, запущен ли PostgreSQL, правильны ли credentials в main.cpp.
- Компилиция не работает: Установите MinGW (для g++) или Visual Studio Community.
- Нет графиков: Убедитесь, что chart.min.js в папке web.
- Порт занят: Измените порт в main.cpp (svr.listen("localhost", 8080);).
- Отчет не скачивается: Проверьте права на запись в папку web (где генерируются CSV-файлы).