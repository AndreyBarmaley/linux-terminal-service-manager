/***************************************************************************
 *   Copyright © 2021 by Andrey Afletdinov <public.irkutsk@gmail.com>      *
 *                                                                         *
 *   Part of the LTSM: Linux Terminal Service Manager:                     *
 *   https://github.com/AndreyBarmaley/linux-terminal-service-manager      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef _LTSM_JSON_WRAPPER_
#define _LTSM_JSON_WRAPPER_

#include <map>
#include <any>
#include <list>
#include <string>
#include <vector>
#include <memory>
#include <sstream>
#include <utility>
#include <typeindex>
#include <filesystem>
#include <string_view>
#include <forward_list>

#include "jsmn/jsmn.h"
#include "ltsm_global.h"

#define LTSM_JSON_WRAPPER 20250811

namespace LTSM
{

    class JsmnToken : protected jsmntok_t
    {
    public:
        JsmnToken();

        const int & counts(void) const;
        const int & start(void) const;
        const int & end(void) const;

        bool isKey(void) const;
        bool isValue(void) const;
        bool isPrimitive(void) const;
        bool isString(void) const;
        bool isArray(void) const;
        bool isObject(void) const;

        const char* typeString(void) const;
    };

    enum class JsonType { Null, Integer, Double, String, Boolean, Object, Array };
    const char* jsonTypeString(const JsonType &);

    class JsonContent;
    class JsonObject;
    class JsonArray;

    /// @brief: base json value interace
    class JsonValue
    {
    public:
        JsonValue() = default;
        virtual ~JsonValue() = default;

        virtual JsonType getType(void) const = 0;
        virtual std::string toString(void) const = 0;
        virtual int getInteger(void) const = 0;
        virtual std::string getString(void) const = 0;
        virtual double getDouble(void) const = 0;
        virtual bool getBoolean(void) const = 0;

        template<typename T>
        T get(void) const { T t; *this >> t; return t; }

        bool isNull(void) const;
        bool isBoolean(void) const;
        bool isInteger(void) const;
        bool isDouble(void) const;
        bool isString(void) const;
        bool isObject(void) const;
        bool isArray(void) const;
    };

    const JsonValue & operator>> (const JsonValue &, int &);
    const JsonValue & operator>> (const JsonValue &, std::string &);
    const JsonValue & operator>> (const JsonValue &, double &);
    const JsonValue & operator>> (const JsonValue &, bool &);

    template<typename T1, typename T2>
    const JsonValue & operator>> (const JsonValue & jv, std::pair<T1, T2> & val)
    {
        return jv >> val.first >> val.second;
    }

    /// @brief: base json null
    class JsonNull : public JsonValue
    {
    public:
        JsonType getType(void) const override;
        std::string toString(void) const override;
        int getInteger(void) const override;
        std::string getString(void) const override;
        double getDouble(void) const override;
        bool getBoolean(void) const override;
    };

    /// @brief: json primitive interface
    class JsonPrimitive : public JsonValue
    {
    protected:
        std::any value;

    public:
        template<typename T>
        explicit JsonPrimitive(const T & v) : value(v) {}
        explicit JsonPrimitive(std::string_view v) : value(std::make_any<std::string>(v)) {}

        std::string toString(void) const override;
    };

    /// @brief: json string
    class JsonString : public JsonPrimitive
    {
    public:
        explicit JsonString(std::string_view val) : JsonPrimitive(val) {}

        JsonType getType(void) const override;
        std::string toString(void) const override;
        int getInteger(void) const override;
        std::string getString(void) const override;
        double getDouble(void) const override;
        bool getBoolean(void) const override;
    };

    /// @brief: json double
    class JsonDouble : public JsonPrimitive
    {
    public:
        explicit JsonDouble(const double & val) : JsonPrimitive(val) {}

        JsonType getType(void) const override;
        int getInteger(void) const override;
        std::string getString(void) const override;
        double getDouble(void) const override;
        bool getBoolean(void) const override;
    };

    /// @brief: json integer
    class JsonInteger : public JsonPrimitive
    {
    public:
        explicit JsonInteger(const int & val) : JsonPrimitive(val) {}

        JsonType getType(void) const override;
        int getInteger(void) const override;
        std::string getString(void) const override;
        double getDouble(void) const override;
        bool getBoolean(void) const override;
    };

    /// @brief: json integer
    class JsonBoolean : public JsonPrimitive
    {
    public:
        explicit JsonBoolean(const bool & val) : JsonPrimitive(val) {}

        JsonType getType(void) const override;
        int getInteger(void) const override;
        std::string getString(void) const override;
        double getDouble(void) const override;
        bool getBoolean(void) const override;
    };

    /// @brief: json container interface
    class JsonContainer : public JsonValue
    {
    public:
        JsonContainer() = default;

        virtual bool isValid(void) const { return false; }

        virtual size_t size(void) const = 0;
        virtual void clear(void) = 0;
    };

    /// @brief: JsonValue pointer
    struct JsonValuePtr : std::unique_ptr<JsonValue>
    {
        JsonValuePtr();
        ~JsonValuePtr() = default;

        explicit JsonValuePtr(int);
        explicit JsonValuePtr(bool);
        explicit JsonValuePtr(double);
        explicit JsonValuePtr(std::string_view);
        explicit JsonValuePtr(const JsonArray &);
        explicit JsonValuePtr(const JsonObject &);
        explicit JsonValuePtr(JsonArray &&);
        explicit JsonValuePtr(JsonObject &&);
        explicit JsonValuePtr(JsonValue*);
        explicit JsonValuePtr(const JsonValuePtr &);
        explicit JsonValuePtr(JsonValuePtr &&) noexcept;

        JsonValuePtr & operator=(const JsonValuePtr &);
        JsonValuePtr & operator=(JsonValuePtr &&) noexcept;

        void assign(const JsonValuePtr &);
    };

    /* JsonArray */
    class JsonArray : public JsonContainer
    {
    private:
        int getInteger(void) const override;
        std::string getString(void) const override;
        double getDouble(void) const override;
        bool getBoolean(void) const override;

    protected:
        std::vector<JsonValuePtr> content;
        friend class JsonContent;

    public:
        JsonArray() = default;
        ~JsonArray() = default;

        JsonArray(const JsonArray &);
        JsonArray(JsonArray && ja) noexcept;

        template<typename T>
        explicit JsonArray(const std::list<T> & list)
        {
            content.reserve(list.size());

            for(const auto & val : list) { content.emplace_back(val); }
        }

        template<typename T>
        explicit JsonArray(const std::vector<T> & list)
        {
            content.reserve(list.size());

            for(const auto & val : list) { content.emplace_back(val); }
        }

        template<typename InputIterator>
        explicit JsonArray(InputIterator it1, InputIterator it2)
        {
            content.reserve(std::distance(it1, it2));

            while(it1 != it2) { content.emplace_back(*it1++); }
        }

        JsonArray & operator=(const JsonArray &);
        JsonArray & operator=(JsonArray && ja) noexcept;

        size_t size(void) const override;
        void clear(void) override;
        JsonType getType(void) const override;

        const JsonValue* getValue(size_t index) const;
        const JsonObject* getObject(size_t index) const;
        const JsonArray* getArray(size_t index) const;

        std::string toString(void) const override;

        int getInteger(size_t index) const;
        std::string getString(size_t index) const;
        double getDouble(size_t index) const;
        bool getBoolean(size_t index) const;
        bool isValid(void) const override;
        bool isValid(size_t index) const;

        void addInteger(const int &);
        void addString(const std::wstring &);
        void addString(std::string_view);
        void addDouble(const double &);
        void addBoolean(const bool &);

        void addArray(const JsonArray &);
        void addObject(const JsonObject &);
        void addArray(JsonArray &&);
        void addObject(JsonObject &&);

        void join(const JsonArray &);
        void swap(JsonArray &) noexcept;

        template<typename T>
        std::vector<T> toStdVector(void) const
        {
            std::vector<T> res;
            res.reserve(content.size());

            for(const auto & ptr : content)
            {
                res.emplace_back(ptr->template get<T>());
            }

            return res;
        }

        template<typename T>
        std::list<T> toStdList(void) const
        {
            std::list<T> res;

            for(const auto & ptr : content)
            {
                res.emplace_back(ptr->template get<T>());
            }

            return res;
        }

        template<typename T>
        std::forward_list<T> toStdListForward(void) const
        {
            std::forward_list<T> res;

            for(const auto & ptr : content)
            {
                res.emplace_front(ptr->template get<T>());
            }

            return res;
        }

        template<typename T>
        const JsonArray & operator>> (std::vector<T> & v) const
        {
            for(const auto & ptr : content)
            {
                v.emplace_back(ptr->template get<T>());
            }

            return *this;
        }

        template<typename T>
        const JsonArray & operator>> (std::list<T> & v) const
        {
            for(const auto & ptr : content)
            {
                v.emplace_back(ptr->template get<T>());
            }

            return *this;
        }
    };

    JsonArray & operator<< (JsonArray &, const char*);
    JsonArray & operator<< (JsonArray &, const std::string &);
    JsonArray & operator<< (JsonArray &, const std::string_view &);
    //
    JsonArray & operator<< (JsonArray &, int);
    JsonArray & operator<< (JsonArray &, double);
    JsonArray & operator<< (JsonArray &, bool);

    /* JsonObject */
    class JsonObject : public JsonContainer
    {
    private:
        int getInteger(void) const override;
        std::string getString(void) const override;
        double getDouble(void) const override;
        bool getBoolean(void) const override;

    protected:
        INTMAP<std::string, JsonValuePtr> content;
        friend class JsonContent;

        template<typename T>
        void addValue(const std::string & key, const T & val)
        {
            auto it = content.find(key);

            if(it != content.end())
            {
                (*it).second = JsonValuePtr(val);
            }
            else
            {
                content.emplace(key, JsonValuePtr(val));
            }
        }

    public:
        JsonObject() = default;
        ~JsonObject() = default;

        JsonObject(const JsonObject &);
        JsonObject(JsonObject && jo) noexcept;

        JsonObject & operator=(const JsonObject &);
        JsonObject & operator=(JsonObject && jo) noexcept;

        size_t size(void) const override;
        void clear(void) override;
        JsonType getType(void) const override;
        bool isValid(void) const override;

        std::string toString(void) const override;
        bool hasKey(std::string_view) const;
        std::list<std::string> keys(void) const;

        void removeKey(const std::string &);

        const JsonValue* getValue(std::string_view) const;
        const JsonObject* getObject(std::string_view) const;
        const JsonArray* getArray(std::string_view) const;

        bool isNull(std::string_view) const;
        bool isBoolean(std::string_view) const;
        bool isInteger(std::string_view) const;
        bool isDouble(std::string_view) const;
        bool isString(std::string_view) const;
        bool isObject(std::string_view) const;
        bool isArray(std::string_view) const;

        JsonType getType(std::string_view) const;
        int getInteger(std::string_view, int def = 0) const;
        std::string getString(std::string_view, std::string_view def = "") const;
        double getDouble(std::string_view, double def = 0) const;
        bool getBoolean(std::string_view, bool def = false) const;

        void addNull(const std::string &);
        void addInteger(const std::string &, const int &);
        void addString(const std::string &, const std::wstring &);
        void addString(const std::string &, std::string_view);
        void addDouble(const std::string &, const double &);
        void addBoolean(const std::string &, const bool &);

        void addArray(const std::string &, const JsonArray &);
        void addArray(const std::string &, JsonArray &&);

        void addObject(const std::string &, const JsonObject &);
        void addObject(const std::string &, JsonObject &&);

        void join(const JsonObject &);
        void swap(JsonObject &) noexcept;

        template<typename T>
        std::map<std::string, T> toStdMap(void) const
        {
            std::map<std::string, T> res;

            for(const auto & [key, ptr] : content)
            {
                res.emplace(key, ptr->template get<T>());
            }

            return res;
        }

        template<typename T>
        std::unordered_map<std::string, T> toStdUnorderedMap(void) const
        {
            std::unordered_map<std::string, T> res;

            for(const auto & [key, ptr] : content)
            {
                res.emplace(key, ptr->template get<T>());
            }

            return res;
        }

        template<typename T>
        std::vector<T> getStdVector(std::string_view key) const
        {
            const JsonArray* jarr = getArray(key);
            return jarr ? jarr->template toStdVector<T>() : std::vector<T>();
        }

        template<typename T>
        std::list<T> getStdList(std::string_view key) const
        {
            const JsonArray* jarr = getArray(key);
            return jarr ? jarr->template toStdList<T>() : std::list<T>();
        }

        template<typename T>
        std::forward_list<T> getStdListForward(std::string_view key) const
        {
            const JsonArray* jarr = getArray(key);
            return jarr ? jarr->template toStdListForward<T>() : std::forward_list<T>();
        }
    };

    /* JsonContent */
    class JsonContent : protected std::vector<JsmnToken>
    {
        std::string content;

    protected:
        std::string_view stringToken(const JsmnToken &) const;
        jsmntok_t* toJsmnTok(void);

        int pushValuesToArray(const const_iterator &, JsonArray &) const;
        int pushValuesToObject(const const_iterator &, JsonObject &) const;

        std::pair<JsonValuePtr, int> getValueArray(const const_iterator &) const;
        std::pair<JsonValuePtr, int> getValueObject(const const_iterator &) const;
        std::pair<JsonValuePtr, int> getValuePrimitive(const const_iterator &) const;
        std::pair<JsonValuePtr, int> getValueFromIter(const const_iterator &) const;

    public:
        JsonContent() = default;

        bool parseString(std::string_view);
        bool parseBinary(const char*, size_t);
        bool readFile(const std::filesystem::path &);

        bool isObject(void) const;
        bool isArray(void) const;

        JsonArray toArray(void) const;
        JsonObject toObject(void) const;

        bool isValid(void) const;
    };

    /* JsonContentString */
    class JsonContentString : public JsonContent
    {
    public:
        explicit JsonContentString(std::string_view);
    };

    /* JsonContentFile */
    class JsonContentFile : public JsonContent
    {
    public:
        explicit JsonContentFile(const std::filesystem::path &);
    };

    /* JsonStream */
    struct json_plain : std::string
    {
        explicit json_plain(std::string && str) noexcept : std::string(str) {}
    };

    class JsonStream
    {
    protected:
        std::ostringstream os;
        bool comma = false;

    public:
        JsonStream() = default;
        virtual ~JsonStream() = default;

        virtual json_plain flush(void) = 0;
        void reset(void) { os.str(""); }
    };

    class JsonObjectStream : public JsonStream
    {
    public:
        JsonObjectStream();

        JsonObjectStream & push(std::string_view, const json_plain &);
        JsonObjectStream & push(std::string_view, const std::string &);
        JsonObjectStream & push(std::string_view, const std::string_view &);
        JsonObjectStream & push(std::string_view, const char*);
        JsonObjectStream & push(std::string_view, int);
        JsonObjectStream & push(std::string_view, size_t);
        JsonObjectStream & push(std::string_view, double);
        JsonObjectStream & push(std::string_view, bool);
        JsonObjectStream & push(std::string_view);

        json_plain flush(void) override;
    };

    class JsonArrayStream : public JsonStream
    {
    public:
        JsonArrayStream();

        template <typename Iterator>
        JsonArrayStream(Iterator it1, Iterator it2)
        {
            os << "[";

            while(it1 != it2)
            {
                push(*it1++);
            }
        }

        JsonArrayStream & push(const json_plain &);
        JsonArrayStream & push(const std::string &);
        JsonArrayStream & push(const std::string_view &);
        JsonArrayStream & push(const char*);
        JsonArrayStream & push(int);
        JsonArrayStream & push(size_t);
        JsonArrayStream & push(double);
        JsonArrayStream & push(bool);
        JsonArrayStream & push(void);

        json_plain flush(void) override;
    };
}

#endif // _LTSM_JSON_WRAPPER_
