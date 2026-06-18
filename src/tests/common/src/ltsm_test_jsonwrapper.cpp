#include <vector>
#include <string>
#include <list>
#include <utility>
#include <string_view>
#include <filesystem>
#include <map>
#include <unordered_map>
#include <fstream>

#include <gtest/gtest.h>

#include "ltsm_application.h"
#include "ltsm_json_wrapper.h"

using namespace LTSM;

TEST(JsonTest, JsmntokWrapperAndEnums) {
    EXPECT_STREQ(jsonTypeString(JsonType::Integer), "integer");
    EXPECT_STREQ(jsonTypeString(JsonType::Object), "object");

    JsmnToken token;
    EXPECT_EQ(token.start(), -1); // default -1
    EXPECT_FALSE(token.isString());
    EXPECT_FALSE(token.isArray());
    
    EXPECT_NE(token.typeString(), nullptr);
}

TEST(JsonTest, PrimitiveTypesAndGetters) {
    JsonNull j_null;
    EXPECT_EQ(j_null.getType(), JsonType::Null);
    EXPECT_TRUE(j_null.isNull());
    EXPECT_EQ(j_null.getInteger(), 0);
    EXPECT_EQ(j_null.toString(), "null");

    JsonString j_str("hello");
    EXPECT_EQ(j_str.getType(), JsonType::String);
    EXPECT_TRUE(j_str.isString());
    EXPECT_EQ(j_str.getString(), "hello");

    JsonInteger j_int(42);
    EXPECT_EQ(j_int.getType(), JsonType::Integer);
    EXPECT_TRUE(j_int.isInteger());
    EXPECT_EQ(j_int.getInteger(), 42);

    JsonDouble j_double(3.14);
    EXPECT_EQ(j_double.getType(), JsonType::Double);
    EXPECT_TRUE(j_double.isDouble());
    EXPECT_DOUBLE_EQ(j_double.getDouble(), 3.14);

    JsonBoolean j_bool(true);
    EXPECT_EQ(j_bool.getType(), JsonType::Boolean);
    EXPECT_TRUE(j_bool.isBoolean());
    EXPECT_TRUE(j_bool.getBoolean());
}

TEST(JsonTest, StreamingOperatorsAndTemplateGet) {
    JsonInteger j_int(100);
    JsonString j_str("LTSM");

    int int_val = 0;
    j_int >> int_val;
    EXPECT_EQ(int_val, 100);

    std::string str_val;
    j_str >> str_val;
    EXPECT_EQ(str_val, "LTSM");

    const JsonValue& base_int = j_int;
    EXPECT_EQ(base_int.get<int>(), 100);

    const JsonValue& base_str = j_str;
    EXPECT_EQ(base_str.get<std::string>(), "LTSM");
}

TEST(JsonTest, ValuePointerOwnership) {
    JsonValuePtr ptr_int(55);
    ASSERT_NE(ptr_int.get(), nullptr);
    EXPECT_EQ(ptr_int->getInteger(), 55);

    JsonValuePtr ptr_str1("text1");
    EXPECT_EQ(ptr_str1->getString(), "text1");

    std::string str("text2");
    JsonValuePtr ptr_str2(str);
    EXPECT_EQ(ptr_str2->getString(), str);

    JsonValuePtr clone; 
    EXPECT_EQ(clone->getInteger(), 0);
    
    clone.assign(JsonValuePtr(99));
    EXPECT_EQ(clone->getInteger(), 99);
    EXPECT_EQ(ptr_int->getInteger(), 55);

    JsonValuePtr moved(std::move(clone));
    EXPECT_EQ(moved->getInteger(), 99);
    EXPECT_EQ(clone.get(), nullptr);
}

TEST(JsonArrayTest, ModificationAndAccess) {
    JsonArray arr;
    EXPECT_EQ(arr.size(), 0);
    EXPECT_FALSE(arr.isValid());

    arr.addInteger(10);
    arr.addString("item");
    arr.addBoolean(false);
    arr.addDouble(2.5);

    EXPECT_EQ(arr.size(), 4);
    EXPECT_TRUE(arr.isValid(0));

    EXPECT_EQ(arr.getInteger(0), 10);
    EXPECT_EQ(arr.getString(1), "item");
    EXPECT_FALSE(arr.getBoolean(2));
    EXPECT_DOUBLE_EQ(arr.getDouble(3), 2.5);

    const JsonValue* val_ptr = arr.getValue(0);
    ASSERT_NE(val_ptr, nullptr);
    EXPECT_TRUE(val_ptr->isInteger());

    arr.clear();
    EXPECT_EQ(arr.size(), 0);
}

TEST(JsonArrayTest, OperatorsAndFluentStreaming) {
    JsonArray arr;
    
    arr << "apple" << 42 << 1.15 << true;
    
    ASSERT_EQ(arr.size(), 4);
    EXPECT_EQ(arr.getString(0), "apple");
    EXPECT_EQ(arr.getInteger(1), 42);
    EXPECT_DOUBLE_EQ(arr.getDouble(2), 1.15);
    EXPECT_TRUE(arr.getBoolean(3));
}

TEST(JsonArrayTest, StdContainersInteroperability) {
    std::vector<int> src_vec = {1, 2, 3};
    JsonArray arr_from_vec(src_vec);
    ASSERT_EQ(arr_from_vec.size(), 3);
    EXPECT_EQ(arr_from_vec.getInteger(1), 2);

    std::vector<int> out_vec = arr_from_vec.toStdVector<int>();
    EXPECT_EQ(out_vec, src_vec);

    std::vector<int> streamed_vec;
    arr_from_vec >> streamed_vec;
    EXPECT_EQ(streamed_vec.size(), 3);
    EXPECT_EQ(streamed_vec[2], 3);
}

TEST(JsonArrayTest, JoinAndSwap) {
    JsonArray arr1;
    arr1 << 1 << 2;

    JsonArray target;
    target << 99;
    
    arr1.swap(target);
    EXPECT_EQ(arr1.size(), 1);
    EXPECT_EQ(arr1.getInteger(0), 99);
    EXPECT_EQ(target.size(), 2);
}

TEST(JsonObjectTest, ConstructionAndLifecycle) {
    JsonObject obj;
    EXPECT_EQ(obj.size(), 0);
    EXPECT_EQ(obj.getType(), JsonType::Object);
    EXPECT_FALSE(obj.isValid());

    obj.addNull("null_key");
    obj.addInteger("int_key", 42);
    obj.addString("str_key", "hello");
    obj.addDouble("double_key", 3.14);
    obj.addBoolean("bool_key", true);

    EXPECT_EQ(obj.size(), 5);

    JsonObject copy_constructed(obj);
    EXPECT_EQ(copy_constructed.size(), 5);
    EXPECT_TRUE(copy_constructed.hasKey("int_key"));

    JsonObject copy_assigned;
    copy_assigned = obj;
    EXPECT_EQ(copy_assigned.size(), 5);

    JsonObject move_constructed(std::move(copy_constructed));
    EXPECT_EQ(move_constructed.size(), 5);

    JsonObject move_assigned;
    move_assigned = std::move(copy_assigned);
    EXPECT_EQ(move_assigned.size(), 5);
}

TEST(JsonObjectTest, KeyValidationAndGetters) {
    JsonObject obj;
    obj.addInteger("id", 101);
    obj.addString("name", "LTSM");
    obj.addNull("meta");

    EXPECT_TRUE(obj.hasKey("id"));
    EXPECT_FALSE(obj.hasKey("unknown"));

    EXPECT_TRUE(obj.isInteger("id"));
    EXPECT_TRUE(obj.isString("name"));
    EXPECT_TRUE(obj.isNull("meta"));
    EXPECT_EQ(obj.getType("id"), JsonType::Integer);

    EXPECT_EQ(obj.getInteger("id", 0), 101);
    EXPECT_EQ(obj.getInteger("unknown", 999), 999); // default

    EXPECT_EQ(obj.getString("name", "def"), "LTSM");
    EXPECT_EQ(obj.getString("unknown", "fallback"), "fallback");

    std::list<std::string> all_keys = obj.keys();
    EXPECT_EQ(all_keys.size(), 3);
    
    obj.removeKey("meta");
    EXPECT_FALSE(obj.hasKey("meta"));
    EXPECT_EQ(obj.size(), 2);
}

TEST(JsonObjectTest, NestedContainersAndStdInteroperability) {
    JsonObject root;
    
    JsonArray arr;
    arr.addInteger(10);
    
    JsonObject child;
    child.addString("status", "ok");

    root.addArray("my_array", arr);
    root.addObject("my_object", child);

    const JsonValue* val = root.getValue("my_array");
    ASSERT_NE(val, nullptr);
    EXPECT_TRUE(val->isArray());

    const JsonArray* arr_ptr = root.getArray("my_array");
    ASSERT_NE(arr_ptr, nullptr);
    EXPECT_EQ(arr_ptr->size(), 1);

    const JsonObject* obj_ptr = root.getObject("my_object");
    ASSERT_NE(obj_ptr, nullptr);
    EXPECT_EQ(obj_ptr->getString("status"), "ok");

    JsonObject flat_obj;
    flat_obj.addString("k1", "v1");
    flat_obj.addString("k2", "v2");

    std::map<std::string, std::string> std_map = flat_obj.toStdMap<std::string>();
    EXPECT_EQ(std_map.size(), 2);
    EXPECT_EQ(std_map["k1"], "v1");
}

TEST(JsonObjectTest, JoinAndSwap) {
    JsonObject obj1;
    obj1.addInteger("a", 1);

    JsonObject obj2;
    obj2.addInteger("b", 2);

    obj1.join(obj2);
    EXPECT_EQ(obj1.size(), 2);
    EXPECT_TRUE(obj1.hasKey("b"));

    JsonObject target;
    target.addInteger("x", 99);

    obj1.swap(std::move(target));
    EXPECT_EQ(obj1.size(), 1);
    EXPECT_TRUE(obj1.hasKey("x"));
}

TEST(JsonContentTest, ParsingFromStringAndBinary) {
    std::string valid_json = R"({"code": 200, "message": "success"})";

    JsonContent content;
    bool parse_ok = content.parseString(std::string(valid_json));
    
    ASSERT_TRUE(parse_ok);
    EXPECT_TRUE(content.isValid());
    EXPECT_TRUE(content.isObject());
    EXPECT_FALSE(content.isArray());

    JsonObject obj = content.toObject();
    EXPECT_EQ(obj.getInteger("code"), 200);
    EXPECT_EQ(obj.getString("message"), "success");

    std::string array_json = "[1, 2, 3]";
    JsonContent bin_content;
    bool bin_ok = bin_content.parseBinary(array_json.data(), array_json.size());
    
    ASSERT_TRUE(bin_ok);
    EXPECT_TRUE(bin_content.isArray());
    JsonArray arr = bin_content.toArray();
    EXPECT_EQ(arr.size(), 3);
}

TEST(JsonContentTest, SpecializedContentClasses) {
    std::string json_str = R"({"status": true})";
    JsonContentString content_str(json_str);
    
    EXPECT_TRUE(content_str.isValid());
    EXPECT_TRUE(content_str.toObject().getBoolean("status"));

    std::filesystem::path temp_file = std::filesystem::temp_directory_path() / "test_parser.json";
    std::string file_data = R"([42, 43])";

    std::ofstream ofs(temp_file);
    ofs << file_data;
    ofs.close();

    JsonContentFile content_file(temp_file);
    EXPECT_TRUE(content_file.isValid());
    EXPECT_TRUE(content_file.isArray());
    EXPECT_EQ(content_file.toArray().size(), 2);

    std::filesystem::remove(temp_file);
}

TEST(JsonStreamTest, JsonPlainAndReset) {
    // json_plain -> std::string
    json_plain plain(std::string("{\"test\":1}"));
    EXPECT_EQ(plain, "{\"test\":1}");
    EXPECT_FALSE(plain.empty());
}

TEST(JsonObjectStreamTest, SerializePrimitivesAndStrings) {
    JsonObjectStream stream;

    stream.push("id", 42);
    stream.push("price", 19.99);
    stream.push("active", true);
    stream.push("name", std::string("LTSM"));
    stream.push("tag", std::string_view("v1.0"));
    stream.push("ctag", "c char");

    json_plain result = stream.flush();
    EXPECT_EQ(result, "{\"id\":42,\"price\":19.99,\"active\":true,\"name\":\"LTSM\",\"tag\":\"v1.0\",\"ctag\":\"c char\"}");
}

TEST(JsonObjectStreamTest, SerializeNullAndCharacters) {
    JsonObjectStream stream;
    stream.push("meta");

    // (sizeof == 1) -> int
    uint8_t byte_val = 15;
    stream.push("byte", byte_val);

    char raw_arr[] = "array_str";
    stream.push("raw_array", raw_arr);

    const char* ptr_valid = "pointer_str";
    const char* ptr_null = nullptr;
    stream.push("ptr_ok", ptr_valid);
    stream.push("ptr_empty", ptr_null);

    json_plain result = stream.flush();

    EXPECT_TRUE(result.find("\"byte\":15") != std::string::npos);
    EXPECT_TRUE(result.find("\"raw_array\":\"array_str\"") != std::string::npos);
    EXPECT_TRUE(result.find("\"ptr_ok\":\"pointer_str\"") != std::string::npos);
    EXPECT_TRUE(result.find("\"ptr_empty\":\"\"") != std::string::npos);
}

TEST(JsonObjectStreamTest, ResetBehavior) {
    JsonObjectStream stream;
    stream.push("a", 1);
    stream.flush();

    stream.reset();
    
    stream.push("b", 2);
    json_plain result = stream.flush();
    EXPECT_EQ(result, "{\"b\":2}");
}

TEST(JsonArrayStreamTest, SerializePrimitives) {
    JsonArrayStream stream;

    stream.push(10);
    stream.push(false);
    stream.push(std::string("apple"));
    stream.push(uint8_t(254)); // sizeof == 1 -> int

    json_plain result = stream.flush();
    EXPECT_EQ(result, "[10,false,\"apple\",254]");
}

TEST(JsonArrayStreamTest, SerializePointersAndNull) {
    JsonArrayStream stream;

    stream.push();

    const char* valid_str = "hello";
    const char* null_str = nullptr;

    stream.push(valid_str);
    stream.push(null_str);

    json_plain result = stream.flush();
    EXPECT_TRUE(result.find("\"hello\"") != std::string::npos);
    EXPECT_TRUE(result.find("\"\"") != std::string::npos);
}

TEST(JsonArrayStreamTest, SerializeRangesAndContainers) {
    std::vector<int> numbers = {1, 2, 3};
    JsonArrayStream stream_from_vector(numbers);

    json_plain res1 = stream_from_vector.flush();
    EXPECT_EQ(res1, "[1,2,3]");

    JsonArrayStream complex_stream;
    std::list<std::string> fruits = {"banana", "cherry"};
    
    complex_stream.push(100);
    complex_stream.push(fruits);
    json_plain res2 = complex_stream.flush();
    EXPECT_EQ(res2, "[100,\"banana\",\"cherry\"]");
}

int main(int argc, char** argv) {
    Application::setDebugTarget(DebugTarget::SyslogFile, "test_jsonwrapper.log");
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
