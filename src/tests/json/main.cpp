#include <fstream>
#include <iostream>
#include <exception>
#include <algorithm>
#include <cassert>

#include "ltsm_tools.h"
#include "ltsm_application.h"
#include "ltsm_json_wrapper.h"

using namespace LTSM;

class Test1App : public Application
{
    JsonObject  config;

public:
    Test1App(const char* ident, int argc, const char** argv) : Application(ident)
    {
        const char* file = 1 < argc ? argv[1] : "test.json";

        JsonContentFile jsonFile(file);

        if(! jsonFile.isValid() || ! jsonFile.isObject())
            throw std::invalid_argument("json parse error");

        config = jsonFile.toObject();
    }

    int start(void) override
    {
        auto arr1 = config.getArray("test:array");
        auto obj1 = config.getObject("test:object");

        std::cout << "test Object::isArray" << std::endl;
        assert(config.isArray("test:array"));
        assert(obj1->isArray("test:arr"));

        std::cout << "test Object::isObject" << std::endl;
        assert(config.isObject("test:object"));
        assert(obj1->isObject("test:obj"));

        auto arr2 = obj1->getArray("test:arr");
        auto obj2 = obj1->getObject("test:obj");

        std::cout << "test Object::isString" << std::endl;
        assert(config.isString("test:string"));
        std::cout << "test Object::isInteger" << std::endl;
        assert(config.isInteger("test:int"));
        std::cout << "test Object::isDouble" << std::endl;
        assert(config.isDouble("test:double"));
        std::cout << "test Object::isBoolean" << std::endl;
        assert(config.isBoolean("test:true"));
        std::cout << "test Object::isNull" << std::endl;
        assert(config.isNull("test:null"));

        std::cout << "test Object::getArray" << std::endl;
        assert(arr1);

        std::cout << "test Object::getObject" << std::endl;
        assert(obj1);

        std::cout << "test Object::getInteger" << std::endl;
        assert(config.getInteger("test:int") == 1234567);
        assert(obj1->getInteger("test:int") == 111);

        std::cout << "test Object::getDouble" << std::endl;
        assert(config.getDouble("test:double") == 1.234567);
        assert(obj1->getDouble("test:double") == 555.6789);

        std::cout << "test Object::getBoolean true" << std::endl;
        assert(config.getBoolean("test:true"));
        assert(! obj1->getBoolean("test:false"));

        std::cout << "test Object::getBoolean false" << std::endl;
        assert(! config.getBoolean("test:false"));

        std::cout << "test Object::keys" << " [" << Tools::join(config.keys(), ",") << "]" << std::endl;
        assert(config.keys().size() == 7);

        std::cout << "test Object::getStdVector<int>" << std::endl;
        assert(config.getStdVector<int>("test:array").size() == 9);

        std::cout << "test Object::getStdList<int>" << std::endl;
        assert(config.getStdList<int>("test:array").size() == 9);

        std::cout << "test Array::getInteger" << std::endl;
        assert(arr1->getInteger(0) == 1 && arr1->getInteger(8) == 9);

        std::cout << "test Array::getString" << std::endl;
        assert(arr1->getString(0) == "1");

        std::cout << "test Array::getBoolean" << std::endl;
        assert(arr1->getBoolean(0));

        std::cout << "test Array::isValid" << std::endl;
        assert(! arr1->isValid(9));

        JsonObject obj;
        obj.addInteger("val1", 111);
        obj.addString("val2", "112");
        obj.addDouble("val3", 113.123);
        obj.addArray("val4", *arr2);
        obj.addObject("val5", *obj2);

        std::cout << "test JsonObject new" << std::endl;
        assert(obj.size() == 5);

        std::cout << obj.toString() << std::endl;

        std::string teststr("errtert");

        JsonArray jarr;
        jarr << "test1" << "test2" << "test3" << "test4" << teststr;
        std::cout << jarr.toString() << std::endl;

        JsonObjectStream jos;
        std::cout << "json stream: " << jos.push("key1", "string").push("key11",teststr).push("key2", 456).push("key3", 3.147).push("key4", true).push("key5").flush() << std::endl;
        return 0;
    }
};

int main(int argc, const char** argv)
{
    Test1App test1("Test1", argc, argv);
    return test1.start();
}
