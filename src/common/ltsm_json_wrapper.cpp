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

#include <tuple>
#include <cctype>
#include <sstream>
#include <fstream>
#include <utility>
#include <algorithm>
#include <stdexcept>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_json_wrapper.h"

namespace LTSM
{
    JsmnToken::JsmnToken()
    {
        jsmntok_t::type = JSMN_PRIMITIVE;
        jsmntok_t::start = -1;
        jsmntok_t::end = -1;
        jsmntok_t::size = 0;
    }

    const int & JsmnToken::counts(void) const
    {
        return jsmntok_t::size;
    }

    const int & JsmnToken::start(void) const
    {
        return jsmntok_t::start;
    }

    const int & JsmnToken::end(void) const
    {
        return jsmntok_t::end;
    }

    bool JsmnToken::isKey(void) const
    {
        return isString() && counts() == 1;
    }

    bool JsmnToken::isValue(void) const
    {
        return isPrimitive() || isObject() || isArray() || (isString() && counts() == 0);
    }

    bool JsmnToken::isPrimitive(void) const
    {
        return jsmntok_t::type == JSMN_PRIMITIVE;
    }

    bool JsmnToken::isString(void) const
    {
        return jsmntok_t::type == JSMN_STRING;
    }

    bool JsmnToken::isArray(void) const
    {
        return jsmntok_t::type == JSMN_ARRAY;
    }

    bool JsmnToken::isObject(void) const
    {
        return jsmntok_t::type == JSMN_OBJECT;
    }

    const char* JsmnToken::typeString(void) const
    {
        switch(jsmntok_t::type)
        {
            case JSMN_PRIMITIVE:
                return "primitive";

            case JSMN_OBJECT:
                return "object";

            case JSMN_ARRAY:
                return "array";

            case JSMN_STRING:
                return "string";

            default:
                break;
        }

        return "unknown";
    }

    const char* jsonTypeString(const JsonType & type)
    {
        switch(type)
        {
            case JsonType::Null:
                return "null";

            case JsonType::Integer:
                return "integer";

            case JsonType::Double:
                return "double";

            case JsonType::String:
                return "string";

            case JsonType::Boolean:
                return "boolean";

            case JsonType::Object:
                return "object";

            case JsonType::Array:
                return "array";
        }

        return "unknown";
    }

    /* JsonValue */
    bool JsonValue::isNull(void) const
    {
        return getType() == JsonType::Null;
    }

    bool JsonValue::isBoolean(void) const
    {
        return getType() == JsonType::Boolean;
    }

    bool JsonValue::isInteger(void) const
    {
        return getType() == JsonType::Integer;
    }

    bool JsonValue::isDouble(void) const
    {
        return getType() == JsonType::Double;
    }

    bool JsonValue::isString(void) const
    {
        return getType() == JsonType::String;
    }

    bool JsonValue::isObject(void) const
    {
        return getType() == JsonType::Object;
    }

    bool JsonValue::isArray(void) const
    {
        return getType() == JsonType::Array;
    }

    JsonArray & operator<< (JsonArray & jv, int st)
    {
        jv.addInteger(st);
        return jv;
    }

    JsonArray & operator<< (JsonArray & jv, const char* st)
    {
        jv.addString(st);
        return jv;
    }

    JsonArray & operator<< (JsonArray & jv, const std::string_view & st)
    {
        jv.addString(st);
        return jv;
    }

    JsonArray & operator<< (JsonArray & jv, const std::string & st)
    {
        jv.addString(st);
        return jv;
    }

    JsonArray & operator<< (JsonArray & jv, double st)
    {
        jv.addDouble(st);
        return jv;
    }

    JsonArray & operator<< (JsonArray & jv, bool st)
    {
        jv.addBoolean(st);
        return jv;
    }


    const JsonValue & operator>> (const JsonValue & jv, int & val)
    {
        val = jv.getInteger();
        return jv;
    }

    const JsonValue & operator>> (const JsonValue & jv, std::string & val)
    {
        val = jv.getString();
        return jv;
    }

    const JsonValue & operator>> (const JsonValue & jv, double & val)
    {
        val = jv.getDouble();
        return jv;
    }

    const JsonValue & operator>> (const JsonValue & jv, bool & val)
    {
        val = jv.getBoolean();
        return jv;
    }

    /* JsonNull */
    JsonType JsonNull::getType(void) const
    {
        return JsonType::Null;
    }

    std::string JsonNull::toString(void) const
    {
        return "null";
    }

    int JsonNull::getInteger(void) const
    {
        return 0;
    }

    std::string JsonNull::getString(void) const
    {
        return "";
    }

    double JsonNull::getDouble(void) const
    {
        return 0;
    }

    bool JsonNull::getBoolean(void) const
    {
        return false;
    }

    /* JsonPrimitive */
    std::string JsonPrimitive::toString(void) const
    {
        return getString();
    }

    /* JsonString */
    JsonType JsonString::getType(void) const
    {
        return JsonType::String;
    }

    int JsonString::getInteger(void) const
    {
        auto content = getString();
        int res = 0;

        try
        {
            res = std::stoi(content, nullptr, 0);
        }
        catch(const std::invalid_argument &)
        {
            Application::error("not number: %s", content.c_str());
        }

        return res;
    }

    std::string JsonString::getString(void) const
    {
        return std::any_cast<std::string>(value);
    }

    double JsonString::getDouble(void) const
    {
        auto content = getString();
        double res = 0;

        try
        {
            res = std::stod(content, nullptr);
        }
        catch(const std::invalid_argument &)
        {
            Application::error("not number: %s", content.c_str());
        }

        return res;
    }

    bool JsonString::getBoolean(void) const
    {
        auto content = getString();

        if(content.compare(0, 4, "fals") == 0)
        {
            return false;
        }

        if(content.compare(0, 4, "true") == 0)
        {
            return true;
        }

        int res = 0;

        try
        {
            res = std::stoi(content, nullptr, 0);
        }
        catch(const std::invalid_argument &)
        {
            Application::error("not boolean: %s", content.c_str());
        }

        return res;
    }

    std::string JsonString::toString(void) const
    {
        return Tools::escaped(getString(), true);
    }

    /* JsonDouble */
    JsonType JsonDouble::getType(void) const
    {
        return JsonType::Double;
    }

    double JsonDouble::getDouble(void) const
    {
        return std::any_cast<double>(value);
    }

    int JsonDouble::getInteger(void) const
    {
        return getDouble();
    }

    std::string JsonDouble::getString(void) const
    {
        return std::to_string(getDouble());
    }

    bool JsonDouble::getBoolean(void) const
    {
        return getDouble();
    }

    /* JsonInteger */
    JsonType JsonInteger::getType(void) const
    {
        return JsonType::Integer;
    }

    int JsonInteger::getInteger(void) const
    {
        return std::any_cast<int>(value);
    }

    std::string JsonInteger::getString(void) const
    {
        return std::to_string(getInteger());
    }

    double JsonInteger::getDouble(void) const
    {
        return getInteger();
    }

    bool JsonInteger::getBoolean(void) const
    {
        return getInteger();
    }

    /* JsonBoolean */
    JsonType JsonBoolean::getType(void) const
    {
        return JsonType::Boolean;
    }

    bool JsonBoolean::getBoolean(void) const
    {
        return std::any_cast<bool>(value);
    }

    int JsonBoolean::getInteger(void) const
    {
        return getBoolean() ? 1 : 0;
    }

    std::string JsonBoolean::getString(void) const
    {
        return getBoolean() ? "true" : "false";
    }

    double JsonBoolean::getDouble(void) const
    {
        return getInteger();
    }

    /* JsonValuePtr */
    JsonValuePtr::JsonValuePtr()
    {
        reset(new JsonNull());
    }

    JsonValuePtr::JsonValuePtr(int v)
    {
        reset(new JsonInteger(v));
    }

    JsonValuePtr::JsonValuePtr(bool v)
    {
        reset(new JsonBoolean(v));
    }

    JsonValuePtr::JsonValuePtr(double v)
    {
        reset(new JsonDouble(v));
    }

    JsonValuePtr::JsonValuePtr(std::string_view v)
    {
        reset(new JsonString(v));
    }

    JsonValuePtr::JsonValuePtr(JsonArray && v)
    {
        reset(new JsonArray(v));
    }

    JsonValuePtr::JsonValuePtr(JsonObject && v)
    {
        reset(new JsonObject(v));
    }

    JsonValuePtr::JsonValuePtr(const JsonArray & v)
    {
        reset(new JsonArray(v));
    }

    JsonValuePtr::JsonValuePtr(const JsonObject & v)
    {
        reset(new JsonObject(v));
    }

    JsonValuePtr::JsonValuePtr(JsonValue* v)
    {
        reset(v);
    }

    JsonValuePtr::JsonValuePtr(const JsonValuePtr & v)
    {
        assign(v);
    }

    JsonValuePtr::JsonValuePtr(JsonValuePtr && v) noexcept
    {
        swap(v);
    }

    JsonValuePtr & JsonValuePtr::operator=(const JsonValuePtr & v)
    {
        assign(v);
        return *this;
    }

    JsonValuePtr & JsonValuePtr::operator=(JsonValuePtr && v) noexcept
    {
        swap(v);
        return *this;
    }

    void JsonValuePtr::assign(const JsonValuePtr & v)
    {
        switch(v->getType())
        {
            default:
            case JsonType::Null:
                reset(new JsonNull());
                break;

            case JsonType::Integer:
                reset(new JsonInteger(*static_cast<JsonInteger*>(v.get())));
                break;

            case JsonType::Boolean:
                reset(new JsonBoolean(*static_cast<JsonBoolean*>(v.get())));
                break;

            case JsonType::Double:
                reset(new JsonDouble(*static_cast<JsonDouble*>(v.get())));
                break;

            case JsonType::String:
                reset(new JsonString(*static_cast<JsonString*>(v.get())));
                break;

            case JsonType::Object:
                reset(new JsonObject(*static_cast<JsonObject*>(v.get())));
                break;

            case JsonType::Array:
                reset(new JsonArray(*static_cast<JsonArray*>(v.get())));
                break;
        }
    }

    /* JsonObject */
    JsonObject::JsonObject(const JsonObject & jo)
    {
        content.insert(jo.content.begin(), jo.content.end());
    }

    JsonObject::JsonObject(JsonObject && jo) noexcept
    {
        content.swap(jo.content);
    }

    JsonObject & JsonObject::operator=(const JsonObject & jo)
    {
        if(this != & jo)
        {
            content.clear();
            content.insert(jo.content.begin(), jo.content.end());
        }

        return *this;
    }

    JsonObject & JsonObject::operator=(JsonObject && jo) noexcept
    {
        content.swap(jo.content);
        return *this;
    }

    bool JsonObject::isValid(void) const
    {
        return ! content.empty();
    }

    size_t JsonObject::size(void) const
    {
        return content.size();
    }

    void JsonObject::clear(void)
    {
        return content.clear();
    }

    JsonType JsonObject::getType(void) const
    {
        return JsonType::Object;
    }

    int JsonObject::getInteger(void) const
    {
        return 0;
    }

    std::string JsonObject::getString(void) const
    {
        return "";
    }

    double JsonObject::getDouble(void) const
    {
        return 0;
    }

    bool JsonObject::getBoolean(void) const
    {
        return false;
    }

    bool JsonObject::hasKey(std::string_view key) const
    {
        return getValue(key);
    }

    std::list<std::string> JsonObject::keys(void) const
    {
        std::list<std::string> res;

        for(auto & [key, value] : content)
        {
            res.push_back(key);
        }

        return res;
    }

    bool JsonObject::isNull(std::string_view key) const
    {
        const JsonValue* jv = getValue(key);
        return !jv || jv->isNull();
    }

    bool JsonObject::isBoolean(std::string_view key) const
    {
        const JsonValue* jv = getValue(key);
        return jv && jv->isBoolean();
    }

    bool JsonObject::isInteger(std::string_view key) const
    {
        const JsonValue* jv = getValue(key);
        return jv && jv->isInteger();
    }

    bool JsonObject::isDouble(std::string_view key) const
    {
        const JsonValue* jv = getValue(key);
        return jv && jv->isDouble();
    }

    bool JsonObject::isString(std::string_view key) const
    {
        const JsonValue* jv = getValue(key);
        return jv && jv->isString();
    }

    bool JsonObject::isObject(std::string_view key) const
    {
        const JsonValue* jv = getValue(key);
        return jv && jv->isObject();
    }

    bool JsonObject::isArray(std::string_view key) const
    {
        const JsonValue* jv = getValue(key);
        return jv && jv->isArray();
    }

    const JsonValue* JsonObject::getValue(std::string_view key) const
    {
        auto it = content.find(std::string(key));
        return it != content.end() ? (*it).second.get() : nullptr;
    }

    JsonType JsonObject::getType(std::string_view key) const
    {
        const JsonValue* jv = getValue(key);
        return jv ? jv->getType() : JsonType::Null;
    }

    int JsonObject::getInteger(std::string_view key, int def) const
    {
        const JsonValue* jv = getValue(key);
        return jv ? jv->getInteger() : def;
    }

    std::string JsonObject::getString(std::string_view key, std::string_view def) const
    {
        const JsonValue* jv = getValue(key);
        return jv ? jv->getString() : std::string(def.data(), def.size());
    }

    double JsonObject::getDouble(std::string_view key, double def) const
    {
        const JsonValue* jv = getValue(key);
        return jv ? jv->getDouble() : def;
    }

    bool JsonObject::getBoolean(std::string_view key, bool def) const
    {
        const JsonValue* jv = getValue(key);
        return jv ? jv->getBoolean() : def;
    }

    void JsonObject::removeKey(const std::string & key)
    {
        content.erase(key);
    }

    const JsonObject* JsonObject::getObject(std::string_view key) const
    {
        auto jv = dynamic_cast<const JsonObject*>(getValue(key));
        return jv;
    }

    const JsonArray* JsonObject::getArray(std::string_view key) const
    {
        auto jv = dynamic_cast<const JsonArray*>(getValue(key));
        return jv;
    }

    std::string JsonObject::toString(void) const
    {
        std::ostringstream os;
        os << "{ ";

        for(auto it = content.begin(); it != content.end(); ++it)
        {
            if((*it).second)
            {
                os << Tools::escaped((*it).first, true) << ": " << (*it).second->toString();

                if(std::next(it) != content.end()) { os << ", "; }
            }
        }

        os << " }";
        return os.str();
    }

    void JsonObject::addNull(const std::string & key)
    {
        auto it = content.find(key);

        if(it != content.end())
        {
            (*it).second = JsonValuePtr();
        }
        else
        {
            content.emplace(key, JsonValuePtr());
        }
    }

    void JsonObject::addInteger(const std::string & key, const int & val)
    {
        addValue<int>(key, val);
    }

    void JsonObject::addString(const std::string & key, const std::wstring & val)
    {
        addValue<std::string_view>(key, Tools::wstring2string(val));
    }

    void JsonObject::addString(const std::string & key, std::string_view val)
    {
        addValue<std::string_view>(key, val);
    }

    void JsonObject::addDouble(const std::string & key, const double & val)
    {
        addValue<double>(key, val);
    }

    void JsonObject::addBoolean(const std::string & key, const bool & val)
    {
        addValue<bool>(key, val);
    }

    void JsonObject::addArray(const std::string & key, JsonArray && val)
    {
        addValue<JsonArray>(key, val);
    }

    void JsonObject::addArray(const std::string & key, const JsonArray & val)
    {
        addValue<JsonArray>(key, val);
    }

    void JsonObject::addObject(const std::string & key, JsonObject && val)
    {
        addValue<JsonObject>(key, val);
    }

    void JsonObject::addObject(const std::string & key, const JsonObject & val)
    {
        addValue<JsonObject>(key, val);
    }

    void JsonObject::swap(JsonObject & jo) noexcept
    {
        content.swap(jo.content);
    }

    void JsonObject::join(const JsonObject & jo)
    {
        for(auto & [key, valptr] : jo.content)
        {
            if(valptr->isArray())
            {
                auto it = content.find(key);

                if(it != content.end() && (*it).second->isArray())
                {
                    static_cast<JsonArray*>((*it).second.get())->join(static_cast<const JsonArray &>(*valptr.get()));
                }
                else
                {
                    content.emplace(key, valptr);
                }
            }
            else if(valptr->isObject())
            {
                auto it = content.find(key);

                if(it != content.end() && (*it).second->isArray())
                {
                    static_cast<JsonObject*>((*it).second.get())->join(static_cast<const JsonObject &>(*valptr.get()));
                }
                else
                {
                    content.emplace(key, valptr);
                }
            }
            else
            {
                auto it = content.find(key);

                if(it != content.end())
                {
                    (*it).second = valptr;
                }
                else
                {
                    content.emplace(key, valptr);
                }
            }
        }
    }

    /* JsonArray */
    int JsonArray::getInteger(void) const
    {
        return 0;
    }

    std::string JsonArray::getString(void) const
    {
        return "";
    }

    double JsonArray::getDouble(void) const
    {
        return 0;
    }

    bool JsonArray::getBoolean(void) const
    {
        return false;
    }

    JsonArray::JsonArray(const JsonArray & ja)
    {
        content.assign(ja.content.begin(), ja.content.end());
    }

    JsonArray::JsonArray(JsonArray && ja) noexcept
    {
        content.swap(ja.content);
    }

    JsonArray & JsonArray::operator=(const JsonArray & ja)
    {
        content.assign(ja.content.begin(), ja.content.end());
        return *this;
    }

    JsonArray & JsonArray::operator=(JsonArray && ja) noexcept
    {
        content.swap(ja.content);
        return *this;
    }

    size_t JsonArray::size(void) const
    {
        return content.size();
    }

    void JsonArray::clear(void)
    {
        return content.clear();
    }

    JsonType JsonArray::getType(void) const
    {
        return JsonType::Array;
    }

    int JsonArray::getInteger(size_t index) const
    {
        return isValid(index) ? content[index]->getInteger() : 0;
    }

    std::string JsonArray::getString(size_t index) const
    {
        return isValid(index) ? content[index]->getString() : "";
    }

    double JsonArray::getDouble(size_t index) const
    {
        return isValid(index) ? content[index]->getDouble() : 0;
    }

    bool JsonArray::getBoolean(size_t index) const
    {
        return isValid(index) ? content[index]->getBoolean() : false;
    }

    bool JsonArray::isValid(void) const
    {
        return ! content.empty();
    }

    bool JsonArray::isValid(size_t index) const
    {
        return index < content.size();
    }

    const JsonValue* JsonArray::getValue(size_t index) const
    {
        return index < content.size() ? content[index].get() : nullptr;
    }

    const JsonObject* JsonArray::getObject(size_t index) const
    {
        auto jo = dynamic_cast<const JsonObject*>(getValue(index));
        return jo;
    }

    const JsonArray* JsonArray::getArray(size_t index) const
    {
        auto ja = dynamic_cast<const JsonArray*>(getValue(index));
        return ja;
    }

    std::string JsonArray::toString(void) const
    {
        std::ostringstream os;
        os << "[ ";

        for(auto it = content.begin(); it != content.end(); ++it)
        {
            os << (*it)->toString();

            if(std::next(it) != content.end()) { os << ", "; }
        }

        os << " ]";
        return os.str();
    }

    void JsonArray::addInteger(const int & val)
    {
        content.emplace_back(val);
    }

    void JsonArray::addString(const std::wstring & val)
    {
        content.emplace_back(Tools::wstring2string(val));
    }

    void JsonArray::addString(std::string_view val)
    {
        content.emplace_back(val);
    }

    void JsonArray::addDouble(const double & val)
    {
        content.emplace_back(val);
    }

    void JsonArray::addBoolean(const bool & val)
    {
        content.emplace_back(val);
    }

    void JsonArray::addArray(JsonArray && val)
    {
        content.emplace_back(val);
    }

    void JsonArray::addArray(const JsonArray & val)
    {
        content.emplace_back(val);
    }

    void JsonArray::addObject(JsonObject && val)
    {
        content.emplace_back(val);
    }

    void JsonArray::addObject(const JsonObject & val)
    {
        content.emplace_back(val);
    }

    void JsonArray::swap(JsonArray & ja) noexcept
    {
        content.swap(ja.content);
    }

    void JsonArray::join(const JsonArray & ja)
    {
        if(content.size() > ja.content.size())
        {
            for(int pos = 0; pos < ja.content.size(); ++pos)
            {
                auto & ptr1 = content[pos];
                auto & ptr2 = ja.content[pos];

                if(ptr2->isArray())
                {
                    if(ptr1->isArray())
                    {
                        static_cast<JsonArray*>(ptr1.get())->join(static_cast<const JsonArray &>(*ptr2.get()));
                    }
                    else
                    {
                        ptr1.assign(ptr2);
                    }
                }
                else if(ptr2->isObject())
                {
                    if(ptr1->isObject())
                    {
                        static_cast<JsonObject*>(ptr1.get())->join(static_cast<const JsonObject &>(*ptr2.get()));
                    }
                    else
                    {
                        ptr1.assign(ptr2);
                    }
                }
            }
        }
        else
        {
            content = ja.content;
        }
    }

    /* JsonContent */
    jsmntok_t* JsonContent::toJsmnTok(void)
    {
        return size() ? reinterpret_cast<jsmntok_t*>(& front()) : nullptr;
    }

    bool JsonContent::isValid(void) const
    {
        return size();
    }

    bool JsonContent::parseString(std::string_view str)
    {
        return parseBinary(str.data(), str.size());
    }

    bool JsonContent::parseBinary(const char* str, size_t len)
    {
        int counts = 0;

        do
        {
            jsmn_parser parser;
            jsmn_init(& parser);
            resize(size() + 128, JsmnToken());
            counts = jsmn_parse(& parser, str, len, toJsmnTok(), size());
        }
        while(counts == JSMN_ERROR_NOMEM);

        if(counts == JSMN_ERROR_INVAL)
        {
            Application::error("%s: %s", __FUNCTION__, "invalid character inside JSON content");
            clear();
            return false;
        }
        else if(counts == JSMN_ERROR_PART)
        {
            Application::error("%s: %s", __FUNCTION__, "the content is not a full JSON packet, more bytes expected");
            clear();
            return false;
        }
        else if(counts < 0)
        {
            Application::error("%s: %s", __FUNCTION__, "unknown error");
            clear();
            return false;
        }

        content.assign(str, len);
        resize(counts);
        return true;
    }

    bool JsonContent::readFile(const std::filesystem::path & file)
    {
        auto str = Tools::fileToString(file);

        if(! str.empty())
        {
            return parseBinary(str.data(), str.size());
        }

        return false;
    }

    std::string_view JsonContent::stringToken(const JsmnToken & tok) const
    {
        if(0 <= tok.start() && tok.start() < tok.end())
        {
            return std::string_view(content.data() + tok.start(), tok.end() - tok.start());
        }

        return {};
    }

    bool JsonContent::isArray(void) const
    {
        return isValid() && front().isArray();
    }

    bool JsonContent::isObject(void) const
    {
        return isValid() && front().isObject();
    }

    std::pair<JsonValuePtr, int>
    JsonContent::getValueArray(const const_iterator & it, JsonContainer* cont) const
    {
        int counts = (*it).counts();
        int skip = 1;
        auto itval = Tools::nextToEnd(it, skip, end());
        JsonArray* arr = cont ? static_cast<JsonArray*>(cont) : new JsonArray();

        while(counts-- && itval != end())
        {
            auto [ptr, count] = getValue(itval, nullptr);

            if(ptr)
            {
                arr->content.emplace_back(std::move(ptr));
            }

            skip += count;
            itval = Tools::nextToEnd(it, skip, end());
        }

        // reset reference
        if(cont)
        {
            arr = nullptr;
        }

        return std::make_pair(JsonValuePtr(arr), skip);
    }

    std::pair<JsonValuePtr, int>
    JsonContent::getValueObject(const const_iterator & it, JsonContainer* cont) const
    {
        int counts = (*it).counts();
        int skip = 1;
        auto itkey = Tools::nextToEnd(it, skip, end());
        auto itval = Tools::nextToEnd(itkey, 1, end());
        JsonObject* obj = cont ? static_cast<JsonObject*>(cont) : new JsonObject();

        while(counts-- && itval != end())
        {
            if(!(*itkey).isKey())
            {
                auto str = stringToken(*itkey);
                Application::error("not key, index: %d, `%.*s'", std::distance(begin(), itkey), str.size(), str.data());
            }

            auto key = Tools::unescaped(stringToken(*itkey));
            auto [ptr, count] = getValue(itval, nullptr);

            if(ptr)
            {
                auto it = obj->content.find(key);

                if(it != obj->content.end())
                {
                    (*it).second = std::move(ptr);
                }
                else
                {
                    obj->content.emplace(key, std::move(ptr));
                }
            }

            skip += 1 + count;
            itkey = Tools::nextToEnd(it, skip, end());
            itval = Tools::nextToEnd(itkey, 1, end());
        }

        // reset reference
        if(cont)
        {
            obj = nullptr;
        }

        return std::make_pair(JsonValuePtr(obj), skip);
    }

    std::pair<JsonValuePtr, int>
    JsonContent::getValuePrimitive(const const_iterator & it, JsonContainer* cont) const
    {
        //auto val = std::string(stringToken(*it));
        auto val = stringToken(*it);

        if(! (*it).isValue())
        {
            Application::error("not value, index: %d, value: `%.*s'", std::distance(begin(), it), val.size(), val.data());
        }

        size_t dotpos = val.find(".");

        try
        {
            if(std::string::npos != dotpos)
            {
                double vald = std::stod(std::string(val));
                return std::make_pair(JsonValuePtr(vald), 1);
            }
            else
            {
                int vali = std::stoi(std::string(val), nullptr, 0);
                return std::make_pair(JsonValuePtr(vali), 1);
            }
        }
        catch(const std::invalid_argument &)
        {
            if(Tools::lower(val).compare(0, 5, "false") == 0)
            {
                return std::make_pair(JsonValuePtr(false), 1);
            }

            if(Tools::lower(val).compare(0, 4, "true") == 0)
            {
                return std::make_pair(JsonValuePtr(true), 1);
            }
        }

        return std::make_pair(JsonValuePtr(), 1);
    }

    std::pair<JsonValuePtr, int>
    JsonContent::getValue(const const_iterator & it, JsonContainer* cont) const
    {
        const JsmnToken & tok = *it;

        if(tok.isArray())
        {
            return getValueArray(it, cont);
        }

        if(tok.isObject())
        {
            return getValueObject(it, cont);
        }

        if(tok.isPrimitive())
        {
            return getValuePrimitive(it, cont);
        }

        auto val = stringToken(tok);

        if(! tok.isValue())
        {
            Application::error("not value, index: %d, value: `%.*s'", std::distance(begin(), it), val.size(), val.data());
        }

        return std::make_pair(JsonValuePtr(Tools::unescaped(val)), 1);
    }

    JsonObject JsonContent::toObject(void) const
    {
        JsonObject res;

        if(isObject())
        {
            getValue(begin(), & res);
        }

        return res;
    }

    JsonArray JsonContent::toArray(void) const
    {
        JsonArray res;

        if(isArray())
        {
            getValue(begin(), & res);
        }

        return res;
    }

    /* JsonContentFile */
    JsonContentFile::JsonContentFile(const std::filesystem::path & file)
    {
        readFile(file);
    }

    /* JsonContentString */
    JsonContentString::JsonContentString(std::string_view str)
    {
        parseString(str);
    }

    /* JsonObjectStream */
    JsonObjectStream::JsonObjectStream()
    {
        os << "{";
    }

    JsonObjectStream & JsonObjectStream::push(std::string_view key, const json_plain & val)
    {
        if(comma) { os << ","; }

        os << std::quoted(key) << ":" << val;
        comma = true;
        return *this;
    }

    JsonObjectStream & JsonObjectStream::push(std::string_view key, const std::string & val)
    {
        if(comma) { os << ","; }

        os << std::quoted(key) << ":" << std::quoted(val);
        comma = true;
        return *this;
    }

    JsonObjectStream & JsonObjectStream::push(std::string_view key, const std::string_view & val)
    {
        if(comma) { os << ","; }

        os << std::quoted(key) << ":" << std::quoted(val);
        comma = true;
        return *this;
    }

    JsonObjectStream & JsonObjectStream::push(std::string_view key, const char* val)
    {
        if(comma) { os << ","; }

        os << std::quoted(key) << ":" << std::quoted(val);
        comma = true;
        return *this;
    }

    JsonObjectStream & JsonObjectStream::push(std::string_view key, size_t val)
    {
        if(comma) { os << ","; }

        os << std::quoted(key) << ":" << val;
        comma = true;
        return *this;
    }

    JsonObjectStream & JsonObjectStream::push(std::string_view key, int val)
    {
        if(comma) { os << ","; }

        os << std::quoted(key) << ":" << val;
        comma = true;
        return *this;
    }

    JsonObjectStream & JsonObjectStream::push(std::string_view key, double val)
    {
        if(comma) { os << ","; }

        os << std::quoted(key) << ":" << val;
        comma = true;
        return *this;
    }

    JsonObjectStream & JsonObjectStream::push(std::string_view key, bool val)
    {
        if(comma) { os << ","; }

        os << std::quoted(key) << ":" << (val ? "true" : "false");
        comma = true;
        return *this;
    }

    JsonObjectStream & JsonObjectStream::push(std::string_view key)
    {
        if(comma) { os << ","; }

        os << std::quoted(key) << ":" << "null";
        comma = true;
        return *this;
    }

    json_plain JsonObjectStream::flush(void)
    {
        os << "}";
        return json_plain(os.str());
    }

    /* JsonArrayStream */
    JsonArrayStream::JsonArrayStream()
    {
        os << "[";
    }

    JsonArrayStream & JsonArrayStream::push(const json_plain & val)
    {
        if(comma) { os << ","; }

        os << val;
        comma = true;
        return *this;
    }

    JsonArrayStream & JsonArrayStream::push(const std::string & val)
    {
        if(comma) { os << ","; }

        os << std::quoted(val);
        comma = true;
        return *this;
    }

    JsonArrayStream & JsonArrayStream::push(const std::string_view & val)
    {
        if(comma) { os << ","; }

        os << std::quoted(val);
        comma = true;
        return *this;
    }

    JsonArrayStream & JsonArrayStream::push(const char* val)
    {
        if(comma) { os << ","; }

        os << std::quoted(val);
        comma = true;
        return *this;
    }

    JsonArrayStream & JsonArrayStream::push(int val)
    {
        if(comma) { os << ","; }

        os << val;
        comma = true;
        return *this;
    }

    JsonArrayStream & JsonArrayStream::push(size_t val)
    {
        if(comma) { os << ","; }

        os << val;
        comma = true;
        return *this;
    }

    JsonArrayStream & JsonArrayStream::push(double val)
    {
        if(comma) { os << ","; }

        os << val;
        comma = true;
        return *this;
    }

    JsonArrayStream & JsonArrayStream::push(bool val)
    {
        if(comma) { os << ","; }

        os << (val ? "true" : "false");
        comma = true;
        return *this;
    }

    JsonArrayStream & JsonArrayStream::push(void)
    {
        if(comma) { os << ","; }

        os << "null";
        comma = true;
        return *this;
    }

    json_plain JsonArrayStream::flush(void)
    {
        os << "]";
        return json_plain(os.str());
    }
}
