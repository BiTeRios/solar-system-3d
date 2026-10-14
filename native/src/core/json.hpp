// Небольшой разборщик JSON для файлов данных игры (bodies.json, dynamics.json и др.).
// Ядро не зависит от Godot, поэтому JSON разбирается здесь, а не через класс JSON движка.
// Числа читаются strtod: значения, записанные Python (repr), восстанавливаются до бита.
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sc {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    static Json parse(const std::string &text, std::string *error = nullptr);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    double num(double fallback = 0.0) const { return type_ == Type::Number ? num_ : fallback; }
    int integer(int fallback = 0) const { return type_ == Type::Number ? static_cast<int>(num_) : fallback; }
    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    const std::string &str() const { return str_; }

    size_t size() const { return type_ == Type::Array ? arr_.size() : (type_ == Type::Object ? obj_.size() : 0); }
    const Json &operator[](size_t i) const;
    const Json &operator[](const std::string &key) const;  // нет ключа — пустой Json (Null)
    bool has(const std::string &key) const { return type_ == Type::Object && obj_.count(key) != 0; }
    const std::map<std::string, Json> &items() const { return obj_; }
    const std::vector<Json> &elements() const { return arr_; }
    // Порядок ключей объекта как в файле.
    const std::vector<std::string> &keys() const { return order_; }

    std::vector<double> numbers() const;  // массив чисел

private:
    friend class JsonParser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Json> arr_;
    std::map<std::string, Json> obj_;
    std::vector<std::string> order_;
};

}  // namespace sc
