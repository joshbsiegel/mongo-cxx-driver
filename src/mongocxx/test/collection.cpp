// Copyright 2015 MongoDB Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <chrono>
#include <vector>

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/stdx/make_unique.hpp>
#include <bsoncxx/stdx/string_view.hpp>
#include <bsoncxx/test_util/catch.hh>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/exception/logic_error.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/exception/query_exception.hpp>
#include <mongocxx/exception/write_exception.hpp>
#include <mongocxx/insert_many_builder.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/read_concern.hpp>
#include <mongocxx/test_util/client_helpers.hh>
#include <mongocxx/write_concern.hpp>

namespace {
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;
using namespace bsoncxx::builder::stream;
using namespace mongocxx;

TEST_CASE("A default constructed collection cannot perform operations", "[collection]") {
    instance::current();

    collection c;
    REQUIRE_THROWS_AS(c.name(), mongocxx::logic_error);
}

TEST_CASE("mongocxx::collection copy constructor", "[collection]") {
    instance::current();

    client client{uri{}};
    database db = client["collection_copy_constructor"];

    SECTION("constructing from valid") {
        collection collection_a = db["a"];
        collection collection_b{collection_a};
        REQUIRE(collection_b);
        REQUIRE(collection_b.name() == stdx::string_view{"a"});
    }

    SECTION("constructing from invalid") {
        collection collection_a;
        collection collection_b{collection_a};
        REQUIRE(!collection_b);
    }
}

TEST_CASE("mongocxx::collection copy assignment operator", "[collection]") {
    instance::current();

    client client{uri{}};
    database db = client["collection_copy_assignment"];

    SECTION("assigning valid to valid") {
        collection collection_a = db["a1"];
        collection collection_b = db["b1"];
        collection_b = collection_a;
        REQUIRE(collection_b);
        REQUIRE(collection_b.name() == stdx::string_view{"a1"});
    }

    SECTION("assigning invalid to valid") {
        collection collection_a;
        collection collection_b = db["b2"];
        collection_b = collection_a;
        REQUIRE(!collection_b);
    }

    SECTION("assigning valid to invalid") {
        collection collection_a = db["a3"];
        collection collection_b;
        collection_b = collection_a;
        REQUIRE(collection_b);
        REQUIRE(collection_b.name() == stdx::string_view{"a3"});
    }

    SECTION("assigning invalid to invalid") {
        collection collection_a;
        collection collection_b;
        collection_b = collection_a;
        REQUIRE(!collection_b);
    }
}

TEST_CASE("collection renaming", "[collection]") {
    instance::current();

    client mongodb_client{uri{}};
    database db = mongodb_client["collection_renaming"];

    auto filter = make_document(kvp("key--------unique", "value"));

    std::string collname{"mongo_cxx_driver"};
    std::string other_collname{"mongo_cxx_again"};

    collection coll = db[collname];
    collection other_coll = db[other_collname];

    coll.drop();
    other_coll.drop();

    coll.insert_one(filter.view());  // Ensure that the collection exists.
    other_coll.insert_one({});

    REQUIRE(coll.name() == stdx::string_view{collname});

    std::string new_name{"mongo_cxx_newname"};
    coll.rename(new_name, false);

    REQUIRE(coll.name() == stdx::string_view{new_name});

    REQUIRE(coll.find_one(filter.view(), {}));

    coll.rename(other_collname, true);
    REQUIRE(coll.name() == stdx::string_view{other_collname});
    REQUIRE(coll.find_one(filter.view(), {}));

    coll.drop();
}

TEST_CASE("collection dropping") {
    instance::current();

    client mongodb_client{uri{}};
    database db = mongodb_client["collection_dropping"];

    std::string collname{"mongo_cxx_driver"};
    collection coll = db[collname];
    coll.insert_one({});  // Ensure that the collection exists.

    REQUIRE_NOTHROW(coll.drop());
}

TEST_CASE("CRUD functionality", "[driver::collection]") {
    instance::current();

    client mongodb_client{uri{}};
    database db = mongodb_client["collection_crud_functionality"];

    auto case_insensitive_collation = document{} << "locale"
                                                 << "en_US"
                                                 << "strength" << 2 << finalize;

    auto noack = write_concern{};
    noack.acknowledge_level(write_concern::level::k_unacknowledged);

    SECTION("insert and read single document", "[collection]") {
        collection coll = db["insert_and_read_one"];
        coll.drop();
        auto b = document{} << "_id" << bsoncxx::oid{} << "x" << 1 << finalize;

        REQUIRE(coll.insert_one(b.view()));

        auto c = document{} << "x" << 1 << finalize;
        REQUIRE(coll.insert_one(c.view()));

        auto cursor = coll.find(b.view());

        std::size_t i = 0;
        for (auto&& x : cursor) {
            REQUIRE(x["_id"].get_oid().value == b.view()["_id"].get_oid().value);
            i++;
        }

        REQUIRE(i == 1);
    }

    SECTION("insert_one returns correct result object", "[collection]") {
        stdx::string_view expected_id{"foo"};

        auto doc = document{} << "_id" << expected_id << finalize;

        SECTION("default write concern returns result") {
            collection coll = db["insert_one_default_write"];
            coll.drop();
            auto result = coll.insert_one(doc.view());
            REQUIRE(result);
            REQUIRE(result->result().inserted_count() == 1);
            REQUIRE(result->inserted_id().type() == bsoncxx::type::k_utf8);
            REQUIRE(result->inserted_id().get_utf8().value == expected_id);
        }

        SECTION("unacknowledged write concern returns disengaged optional", "[collection]") {
            collection coll = db["insert_one_unack_write"];
            coll.drop();
            options::insert opts{};
            opts.write_concern(noack);

            auto result = coll.insert_one(doc.view(), opts);
            REQUIRE(!result);

            // Block until server has received the write request, to prevent
            // this unacknowledged write from racing with writes to this
            // collection from other sections.
            db.run_command(document{} << "getLastError" << 1 << finalize);

            auto count = coll.count({});
            REQUIRE(count == 1);
        }
    }

    SECTION("insert and read multiple documents", "[collection]") {
        collection coll = db["insert_and_read_multi"];
        coll.drop();
        document b1;
        document b2;
        document b3;
        document b4;

        b1 << "_id" << bsoncxx::oid{} << "x" << 1;
        b2 << "x" << 2;
        b3 << "x" << 3;
        b4 << "_id" << bsoncxx::oid{} << "x" << 4;

        std::vector<bsoncxx::document::view> docs{};
        docs.push_back(b1.view());
        docs.push_back(b2.view());
        docs.push_back(b3.view());
        docs.push_back(b4.view());

        auto result = coll.insert_many(docs, options::insert{});
        auto cursor = coll.find({});

        SECTION("result count is correct") {
            REQUIRE(result);
            REQUIRE(result->inserted_count() == 4);
        }

        SECTION("read inserted values with range-for") {
            std::int32_t i = 0;
            for (auto&& x : cursor) {
                i++;
                REQUIRE(x["x"].get_int32() == i);
            }

            REQUIRE(i == 4);
        }

        SECTION("multiple iterators move in lockstep") {
            auto end = cursor.end();
            REQUIRE(cursor.begin() != end);

            auto iter1 = cursor.begin();
            auto iter2 = cursor.begin();
            REQUIRE(iter1 == iter2);
            REQUIRE(*iter1 == *iter2);
            iter1++;
            REQUIRE(iter1 == iter2);
            REQUIRE(iter1 != end);
            REQUIRE(*iter1 == *iter2);
        }
    }

    SECTION("insert_many returns correct result object", "[collection]") {
        document b1;
        document b2;

        b1 << "_id"
           << "foo"
           << "x" << 1;
        b2 << "x" << 2;

        std::vector<bsoncxx::document::view> docs{};
        docs.push_back(b1.view());
        docs.push_back(b2.view());

        SECTION("default write concern returns result") {
            collection coll = db["insert_many_default_write"];
            coll.drop();
            auto result = coll.insert_many(docs);

            REQUIRE(result);

            // Verify result->result() is correct:
            REQUIRE(result->result().inserted_count() == 2);

            // Verify result->inserted_count() is correct:
            REQUIRE(result->inserted_count() == 2);

            // Verify result->inserted_ids() is correct:
            auto id_map = result->inserted_ids();
            REQUIRE(id_map[0].type() == bsoncxx::type::k_utf8);
            REQUIRE(id_map[0].get_utf8().value == stdx::string_view{"foo"});
            REQUIRE(id_map[1].type() == bsoncxx::type::k_oid);
            auto second_inserted_doc = coll.find_one(document{} << "x" << 2 << finalize);
            REQUIRE(second_inserted_doc);
            REQUIRE(second_inserted_doc->view()["_id"]);
            REQUIRE(second_inserted_doc->view()["_id"].type() == bsoncxx::type::k_oid);
            REQUIRE(id_map[1].get_oid().value ==
                    second_inserted_doc->view()["_id"].get_oid().value);
        }

        SECTION("unacknowledged write concern returns disengaged optional") {
            collection coll = db["insert_many_unack_write"];
            coll.drop();
            options::insert opts{};
            opts.write_concern(noack);

            auto result = coll.insert_many(docs, opts);
            REQUIRE(!result);

            // Block until server has received the write request, to prevent
            // this unacknowledged write from racing with writes to this
            // collection from other sections.
            db.run_command(document{} << "getLastError" << 1 << finalize);
        }
    }

    SECTION("find does not leak on error", "[collection]") {
        collection coll = db["find_error_no_leak"];
        coll.drop();
        auto find_opts = options::find{}.max_await_time(std::chrono::milliseconds{-1});

        REQUIRE_THROWS_AS(coll.find({}, find_opts), logic_error);
    }

    SECTION("find with collation", "[collection]") {
        collection coll = db["find_with_collation"];
        coll.drop();
        auto b = document{} << "x"
                            << "foo" << finalize;
        REQUIRE(coll.insert_one(b.view()));

        auto predicate = document{} << "x"
                                    << "FOO" << finalize;
        auto find_opts = options::find{}.collation(case_insensitive_collation.view());
        auto cursor = coll.find(predicate.view(), find_opts);
        if (test_util::supports_collation(mongodb_client)) {
            REQUIRE(std::distance(cursor.begin(), cursor.end()) == 1);
        } else {
            REQUIRE_THROWS_AS(std::distance(cursor.begin(), cursor.end()), query_exception);
        }
    }

    SECTION("find_one with collation", "[collection]") {
        collection coll = db["find_one_with_collation"];
        coll.drop();
        auto b = document{} << "x"
                            << "foo" << finalize;
        REQUIRE(coll.insert_one(b.view()));

        auto predicate = document{} << "x"
                                    << "FOO" << finalize;
        auto find_opts = options::find{}.collation(case_insensitive_collation.view());
        if (test_util::supports_collation(mongodb_client)) {
            REQUIRE(coll.find_one(predicate.view(), find_opts));
        } else {
            REQUIRE_THROWS_AS(coll.find_one(predicate.view(), find_opts), query_exception);
        }
    }

    SECTION("insert and update single document", "[collection]") {
        collection coll = db["insert_and_update_one"];
        coll.drop();
        auto b1 = document{} << "_id" << 1 << finalize;

        coll.insert_one(b1.view());

        auto doc = coll.find_one({});
        REQUIRE(doc);
        REQUIRE(doc->view()["_id"].get_int32() == 1);

        document update_doc;
        update_doc << "$set" << open_document << "changed" << true << close_document;

        coll.update_one(b1.view(), update_doc.view());

        auto updated = coll.find_one({});
        REQUIRE(updated);
        REQUIRE(updated->view()["changed"].get_bool() == true);
    }

    SECTION("update_one returns correct result object", "[collection]") {
        auto b1 = document{} << "_id" << 1 << finalize;

        document update_doc;
        update_doc << "$set" << open_document << "changed" << true << close_document;

        SECTION("default write concern returns result") {
            collection coll = db["update_one_default_write"];
            coll.drop();

            coll.insert_one(b1.view());

            auto result = coll.update_one(b1.view(), update_doc.view());
            REQUIRE(result);
            REQUIRE(result->result().matched_count() == 1);
        }

        SECTION("unacknowledged write concern returns disengaged optional") {
            collection coll = db["update_one_unack_write"];
            coll.drop();

            coll.insert_one(b1.view());
            options::update opts{};
            opts.write_concern(noack);

            auto result = coll.update_one(b1.view(), update_doc.view(), opts);
            REQUIRE(!result);

            // Block until server has received the write request, to prevent
            // this unacknowledged write from racing with writes to this
            // collection from other sections.
            db.run_command(document{} << "getLastError" << 1 << finalize);
        }
    }

    SECTION("update_one with collation", "[collection]") {
        collection coll = db["update_one_with_collation"];
        coll.drop();
        auto b = document{} << "x"
                            << "foo" << finalize;
        REQUIRE(coll.insert_one(b.view()));

        auto predicate = document{} << "x"
                                    << "FOO" << finalize;

        document update_doc;
        update_doc << "$set" << open_document << "changed" << true << close_document;

        auto update_opts = options::update{}.collation(case_insensitive_collation.view());
        if (test_util::supports_collation(mongodb_client)) {
            auto result = coll.update_one(predicate.view(), update_doc.view(), update_opts);
            REQUIRE(result);
            REQUIRE(result->modified_count() == 1);
        } else {
            REQUIRE_THROWS_AS(coll.update_one(predicate.view(), update_doc.view(), update_opts),
                              bulk_write_exception);
        }
    }

    SECTION("insert and update multiple documents", "[collection]") {
        collection coll = db["insert_and_update_multi"];
        coll.drop();
        auto b1 = document{} << "x" << 1 << finalize;

        coll.insert_one(b1.view());
        coll.insert_one(b1.view());

        auto b2 = document{} << "x" << 2 << finalize;

        coll.insert_one(b2.view());

        REQUIRE(coll.count(b1.view()) == 2);

        document bchanged;
        bchanged << "changed" << true;

        document update_doc;
        update_doc << "$set" << bsoncxx::types::b_document{bchanged};

        coll.update_many(b1.view(), update_doc.view());

        REQUIRE(coll.count(bchanged.view()) == 2);
    }

    SECTION("update_many returns correct result object", "[collection]") {
        auto b1 = document{} << "x" << 1 << finalize;

        document bchanged;
        bchanged << "changed" << true;

        document update_doc;
        update_doc << "$set" << bsoncxx::types::b_document{bchanged};

        SECTION("default write concern returns result") {
            collection coll = db["update_many_default_write"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            auto result = coll.update_many(b1.view(), update_doc.view());
            REQUIRE(result);
            REQUIRE(result->result().matched_count() == 2);
        }

        SECTION("unacknowledged write concern returns disengaged optional") {
            collection coll = db["update_many_unack_write"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());
            options::update opts{};
            opts.write_concern(noack);

            auto result = coll.update_many(b1.view(), update_doc.view(), opts);
            REQUIRE(!result);

            // Block until server has received the write request, to prevent
            // this unacknowledged write from racing with writes to this
            // collection from other sections.
            db.run_command(document{} << "getLastError" << 1 << finalize);
        }
    }

    SECTION("update_many with collation", "[collection]") {
        collection coll = db["update_many_with_collation"];
        coll.drop();
        auto b = document{} << "x"
                            << "foo" << finalize;
        REQUIRE(coll.insert_one(b.view()));

        auto predicate = document{} << "x"
                                    << "FOO" << finalize;

        document update_doc;
        update_doc << "$set" << open_document << "changed" << true << close_document;

        auto update_opts = options::update{}.collation(case_insensitive_collation.view());
        if (test_util::supports_collation(mongodb_client)) {
            auto result = coll.update_many(predicate.view(), update_doc.view(), update_opts);
            REQUIRE(result);
            REQUIRE(result->modified_count() == 1);
        } else {
            REQUIRE_THROWS_AS(coll.update_many(predicate.view(), update_doc.view(), update_opts),
                              bulk_write_exception);
        }
    }

    SECTION("replace document replaces only one document", "[collection]") {
        collection coll = db["replace_one_only_one"];
        coll.drop();
        document doc;
        doc << "x" << 1;

        coll.insert_one(doc.view());
        coll.insert_one(doc.view());

        REQUIRE(coll.count(doc.view()) == 2);

        document replacement;
        replacement << "x" << 2;

        coll.replace_one(doc.view(), replacement.view());
        REQUIRE(coll.count(doc.view()) == 1);
        REQUIRE(coll.count(replacement.view()) == 1);
    }

    SECTION("non-matching upsert creates document", "[collection]") {
        collection coll = db["non_match_upsert_creates_doc"];
        coll.drop();
        document b1;
        b1 << "_id" << 1;

        document update_doc;
        update_doc << "$set" << open_document << "changed" << true << close_document;

        options::update options;
        options.upsert(true);

        auto result = coll.update_one(b1.view(), update_doc.view(), options);
        REQUIRE(result->upserted_id());

        auto updated = coll.find_one({});

        REQUIRE(updated);
        REQUIRE(updated->view()["changed"].get_bool() == true);
        REQUIRE(coll.count({}) == (std::int64_t)1);
    }

    SECTION("matching upsert updates document", "[collection]") {
        collection coll = db["match_upsert_updates_doc"];
        coll.drop();
        document b1;
        b1 << "_id" << 1;

        coll.insert_one(b1.view());

        document update_doc;
        update_doc << "$set" << open_document << "changed" << true << close_document;

        options::update options;
        options.upsert(true);

        auto result = coll.update_one(b1.view(), update_doc.view(), options);
        REQUIRE(!(result->upserted_id()));

        auto updated = coll.find_one({});

        REQUIRE(updated);
        REQUIRE(updated->view()["changed"].get_bool() == true);
        REQUIRE(coll.count({}) == 1);
    }

    SECTION("test using an insert_many_builder on this collection", "[collection]") {
        collection coll = db["insert_many_builder_test"];
        coll.drop();
        auto doc_value = document{} << "x" << 1 << finalize;
        auto doc_view = doc_value.view();

        insert_many_builder insert_many{options::insert()};
        insert_many(doc_view);
        insert_many(doc_view);
        insert_many(doc_view);

        insert_many.insert(&coll);

        coll.insert_one(document{} << "b" << 1 << finalize);

        REQUIRE(coll.count(doc_view) == 3);
        REQUIRE(coll.count({}) == 4);
    }

    SECTION("count with hint", "[collection]") {
        collection coll = db["count_with_hint"];
        coll.drop();
        options::count count_opts;
        count_opts.hint(hint{"index_doesnt_exist"});

        auto doc = document{} << "x" << 1 << finalize;
        coll.insert_one(doc.view());

        if (test_util::get_max_wire_version(mongodb_client) >= 2) {
            REQUIRE_THROWS_AS(coll.count(doc.view(), count_opts), operation_exception);
        } else {
            // Old server versions ignore hint sent with count.
            REQUIRE(1 == coll.count(doc.view(), count_opts));
        }
    }

    SECTION("count with collation", "[collection]") {
        collection coll = db["count_with_collation"];
        coll.drop();
        auto doc = document{} << "x"
                              << "foo" << finalize;
        REQUIRE(coll.insert_one(doc.view()));

        auto predicate = document{} << "x"
                                    << "FOO" << finalize;
        auto count_opts = options::count{}.collation(case_insensitive_collation.view());
        if (test_util::supports_collation(mongodb_client)) {
            REQUIRE(coll.count(predicate.view(), count_opts) == 1);
        } else {
            REQUIRE_THROWS_AS(coll.count(predicate.view(), count_opts), query_exception);
        }
    }

    SECTION("replace_one returns correct result object", "[collection]") {
        document b1;
        b1 << "x" << 1;

        document b2;
        b2 << "x" << 2;

        SECTION("default write concern returns result") {
            collection coll = db["replace_one_default_write"];
            coll.drop();

            coll.insert_one(b1.view());

            auto result = coll.replace_one(b1.view(), b2.view());
            REQUIRE(result);
            REQUIRE(result->result().matched_count() == 1);
        }

        SECTION("unacknowledged write concern returns disengaged optional") {
            collection coll = db["replace_one_unack_write"];
            coll.drop();

            coll.insert_one(b1.view());
            options::update opts{};
            opts.write_concern(noack);

            auto result = coll.replace_one(b1.view(), b2.view(), opts);
            REQUIRE(!result);

            // Block until server has received the write request, to prevent
            // this unacknowledged write from racing with writes to this
            // collection from other sections.
            db.run_command(document{} << "getLastError" << 1 << finalize);
        }
    }

    SECTION("replace_one with collation", "[collection]") {
        collection coll = db["replace_one_with_collation"];
        coll.drop();
        document doc;
        doc << "x"
            << "foo";
        REQUIRE(coll.insert_one(doc.view()));

        document predicate;
        predicate << "x"
                  << "FOO";

        document replacement_doc;
        replacement_doc << "x"
                        << "bar";

        auto update_opts = options::update{}.collation(case_insensitive_collation.view());
        if (test_util::supports_collation(mongodb_client)) {
            auto result = coll.replace_one(predicate.view(), replacement_doc.view(), update_opts);
            REQUIRE(result);
            REQUIRE(result->modified_count() == 1);
        } else {
            REQUIRE_THROWS_AS(
                coll.replace_one(predicate.view(), replacement_doc.view(), update_opts),
                bulk_write_exception);
        }
    }

    SECTION("filtered document delete one works", "[collection]") {
        collection coll = db["filtered_doc_delete_one"];
        coll.drop();
        document b1;
        b1 << "x" << 1;

        coll.insert_one(b1.view());

        document b2;
        b2 << "x" << 2;

        coll.insert_one(b2.view());
        coll.insert_one(b2.view());

        REQUIRE(coll.count({}) == 3);

        coll.delete_one(b2.view());

        REQUIRE(coll.count({}) == (std::int64_t)2);

        auto cursor = coll.find({});

        std::int32_t seen = 0;
        for (auto&& x : cursor) {
            seen |= x["x"].get_int32();
        }

        REQUIRE(seen == 3);

        coll.delete_one(b2.view());

        REQUIRE(coll.count({}) == 1);

        cursor = coll.find({});

        seen = 0;
        for (auto&& x : cursor) {
            seen |= x["x"].get_int32();
        }

        REQUIRE(seen == 1);

        coll.delete_one(b2.view());

        REQUIRE(coll.count({}) == 1);

        cursor = coll.find({});

        seen = 0;
        for (auto&& x : cursor) {
            seen |= x["x"].get_int32();
        }

        REQUIRE(seen == 1);
    }

    SECTION("delete_one returns correct result object", "[collection]") {
        document b1;
        b1 << "x" << 1;

        SECTION("default write concern returns result") {
            collection coll = db["delete_one_default_write"];
            coll.drop();

            coll.insert_one(b1.view());

            auto result = coll.delete_one(b1.view());
            REQUIRE(result);
            REQUIRE(result->result().deleted_count() == 1);
        }

        SECTION("unacknowledged write concern returns disengaged optional") {
            collection coll = db["delete_one_unack_write"];
            coll.drop();

            coll.insert_one(b1.view());
            options::delete_options opts{};
            opts.write_concern(noack);

            auto result = coll.delete_one(b1.view(), opts);
            REQUIRE(!result);

            // Block until server has received the write request, to prevent
            // this unacknowledged write from racing with writes to this
            // collection from other sections.
            db.run_command(document{} << "getLastError" << 1 << finalize);
        }
    }

    SECTION("delete_one with collation", "[collection]") {
        collection coll = db["delete_one_with_collation"];
        coll.drop();
        document b1;
        b1 << "x"
           << "foo";

        REQUIRE(coll.insert_one(b1.view()));

        auto predicate = document{} << "x"
                                    << "FOO" << finalize;

        auto delete_opts = options::delete_options{}.collation(case_insensitive_collation.view());
        if (test_util::supports_collation(mongodb_client)) {
            auto result = coll.delete_one(predicate.view(), delete_opts);
            REQUIRE(result);
            REQUIRE(result->deleted_count() == 1);
        } else {
            REQUIRE_THROWS_AS(coll.delete_one(predicate.view(), delete_opts), bulk_write_exception);
        }
    }

    SECTION("delete many works", "[collection]") {
        collection coll = db["delete_many"];
        coll.drop();
        document b1;
        b1 << "x" << 1;

        coll.insert_one(b1.view());

        document b2;
        b2 << "x" << 2;

        coll.insert_one(b2.view());
        coll.insert_one(b2.view());

        REQUIRE(coll.count({}) == 3);

        coll.delete_many(b2.view());

        REQUIRE(coll.count({}) == 1);

        auto cursor = coll.find({});

        std::int32_t seen = 0;
        for (auto&& x : cursor) {
            seen |= x["x"].get_int32();
        }

        REQUIRE(seen == 1);

        coll.delete_many(b2.view());

        REQUIRE(coll.count({}) == 1);

        cursor = coll.find({});

        seen = 0;
        for (auto&& x : cursor) {
            seen |= x["x"].get_int32();
        }

        REQUIRE(seen == 1);
    }

    SECTION("delete_many returns correct result object", "[collection]") {
        document b1;
        b1 << "x" << 1;

        SECTION("default write concern returns result") {
            collection coll = db["delete_many_default_write"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            auto result = coll.delete_many(b1.view());
            REQUIRE(result);
            REQUIRE(result->result().deleted_count() > 1);
        }

        SECTION("unacknowledged write concern returns disengaged optional") {
            collection coll = db["delete_many_unack_write"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());
            coll.insert_one(b1.view());
            options::delete_options opts{};
            opts.write_concern(noack);

            auto result = coll.delete_many(b1.view(), opts);
            REQUIRE(!result);

            // Block until server has received the write request, to prevent
            // this unacknowledged write from racing with writes to this
            // collection from other sections.
            db.run_command(document{} << "getLastError" << 1 << finalize);
        }
    }

    SECTION("delete_many with collation", "[collection]") {
        collection coll = db["delete_many_with_collation"];
        coll.drop();
        document b1;
        b1 << "x"
           << "foo";

        REQUIRE(coll.insert_one(b1.view()));

        auto predicate = document{} << "x"
                                    << "FOO" << finalize;

        auto delete_opts = options::delete_options{}.collation(case_insensitive_collation.view());
        if (test_util::supports_collation(mongodb_client)) {
            auto result = coll.delete_many(predicate.view(), delete_opts);
            REQUIRE(result);
            REQUIRE(result->deleted_count() == 1);
        } else {
            REQUIRE_THROWS_AS(coll.delete_many(predicate.view(), delete_opts),
                              bulk_write_exception);
        }
    }

    SECTION("find works with sort", "[collection]") {
        collection coll = db["find_with_sort"];
        coll.drop();
        document b1;
        b1 << "x" << 1;

        document b2;
        b2 << "x" << 2;

        document b3;
        b3 << "x" << 3;

        coll.insert_one(b1.view());
        coll.insert_one(b3.view());
        coll.insert_one(b2.view());

        SECTION("sort ascending") {
            document sort;
            sort << "x" << 1;
            options::find opts{};
            opts.sort(sort.view());

            auto cursor = coll.find({}, opts);

            std::int32_t x = 1;
            for (auto&& doc : cursor) {
                REQUIRE(x == doc["x"].get_int32());
                x++;
            }
        }

        SECTION("sort descending") {
            document sort;
            sort << "x" << -1;
            options::find opts{};
            opts.sort(sort.view());

            auto cursor = coll.find({}, opts);

            std::int32_t x = 3;
            for (auto&& doc : cursor) {
                REQUIRE(x == doc["x"].get_int32());
                x--;
            }
        }
    }

    SECTION("find_one_and_replace works", "[collection]") {
        document b1;
        b1 << "x"
           << "foo";

        document criteria;
        document replacement;

        criteria << "x"
                 << "foo";
        replacement << "x"
                    << "bar";

        SECTION("without return replacement returns original") {
            collection coll = db["find_one_and_replace_no_return"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            auto doc = coll.find_one_and_replace(criteria.view(), replacement.view());
            REQUIRE(doc);
            REQUIRE(doc->view()["x"].get_utf8().value == stdx::string_view{"foo"});
        }

        SECTION("with return replacement returns new") {
            collection coll = db["find_one_and_replace_return"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            options::find_one_and_replace options;
            options.return_document(options::return_document::k_after);
            auto doc = coll.find_one_and_replace(criteria.view(), replacement.view(), options);
            REQUIRE(doc);
            REQUIRE(doc->view()["x"].get_utf8().value == stdx::string_view{"bar"});
        }

        SECTION("with collation") {
            collection coll = db["find_one_and_replace_with_collation"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            options::find_one_and_replace options;
            options.collation(case_insensitive_collation.view());

            document collation_criteria;
            collation_criteria << "x"
                               << "FOO";

            if (test_util::supports_collation(mongodb_client)) {
                auto doc = coll.find_one_and_replace(
                    collation_criteria.view(), replacement.view(), options);
                REQUIRE(doc);
                REQUIRE(doc->view()["x"].get_utf8().value == stdx::string_view{"foo"});
            } else {
                REQUIRE_THROWS_AS(coll.find_one_and_replace(
                                      collation_criteria.view(), replacement.view(), options),
                                  write_exception);
            }
        }

        SECTION("bad criteria returns negative optional") {
            collection coll = db["find_one_and_replace_bad_criteria"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            document bad_criteria;
            bad_criteria << "x"
                         << "baz";

            auto doc = coll.find_one_and_replace(bad_criteria.view(), replacement.view());

            REQUIRE(!doc);
        }
    }

    SECTION("find_one_and_update works", "[collection]") {
        document b1;
        b1 << "x"
           << "foo";

        document criteria;
        document update;

        criteria << "x"
                 << "foo";
        update << "$set" << open_document << "x"
               << "bar" << close_document;

        SECTION("without return update returns original") {
            collection coll = db["find_one_and_update_no_return"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            auto doc = coll.find_one_and_update(criteria.view(), update.view());

            REQUIRE(doc);

            REQUIRE(doc->view()["x"].get_utf8().value == stdx::string_view{"foo"});
        }

        SECTION("with return update returns new") {
            collection coll = db["find_one_and_update_return"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            options::find_one_and_update options;
            options.return_document(options::return_document::k_after);
            auto doc = coll.find_one_and_update(criteria.view(), update.view(), options);
            REQUIRE(doc);
            REQUIRE(doc->view()["x"].get_utf8().value == stdx::string_view{"bar"});
        }

        SECTION("with collation") {
            collection coll = db["find_one_and_update_with collation"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            options::find_one_and_update options;
            options.collation(case_insensitive_collation.view());

            document collation_criteria;
            collation_criteria << "x"
                               << "FOO";

            if (test_util::supports_collation(mongodb_client)) {
                auto doc =
                    coll.find_one_and_update(collation_criteria.view(), update.view(), options);
                REQUIRE(doc);
                REQUIRE(doc->view()["x"].get_utf8().value == stdx::string_view{"foo"});
            } else {
                REQUIRE_THROWS_AS(
                    coll.find_one_and_update(collation_criteria.view(), update.view(), options),
                    write_exception);
            }
        }

        SECTION("bad criteria returns negative optional") {
            collection coll = db["find_one_and_update_bad_criteria"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            document bad_criteria;
            bad_criteria << "x"
                         << "baz";

            auto doc = coll.find_one_and_update(bad_criteria.view(), update.view());

            REQUIRE(!doc);
        }
    }

    SECTION("find_one_and_delete works", "[collection]") {
        document b1;
        b1 << "x"
           << "foo";

        document criteria;

        criteria << "x"
                 << "foo";

        SECTION("delete one deletes one and returns it") {
            collection coll = db["find_one_and_delete_one"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            auto doc = coll.find_one_and_delete(criteria.view());

            REQUIRE(doc);

            REQUIRE(doc->view()["x"].get_utf8().value == stdx::string_view{"foo"});
            REQUIRE(coll.count({}) == 1);
        }

        SECTION("with collation") {
            collection coll = db["find_one_and_delete_with_collation"];
            coll.drop();

            coll.insert_one(b1.view());
            coll.insert_one(b1.view());

            REQUIRE(coll.count({}) == 2);

            options::find_one_and_delete options;
            options.collation(case_insensitive_collation.view());

            document collation_criteria;
            collation_criteria << "x"
                               << "FOO";

            if (test_util::supports_collation(mongodb_client)) {
                auto doc = coll.find_one_and_delete(collation_criteria.view(), options);
                REQUIRE(doc);
                REQUIRE(doc->view()["x"].get_utf8().value == stdx::string_view{"foo"});
            } else {
                REQUIRE_THROWS_AS(coll.find_one_and_delete(collation_criteria.view(), options),
                                  write_exception);
            }
        }
    }

    SECTION("aggregation", "[collection]") {
        pipeline pipeline;

        auto get_results = [](cursor&& cursor) {
            std::vector<bsoncxx::document::value> results;
            std::transform(cursor.begin(),
                           cursor.end(),
                           std::back_inserter(results),
                           [](bsoncxx::document::view v) { return bsoncxx::document::value{v}; });
            return results;
        };

        SECTION("add_fields") {
            collection coll = db["aggregation_add_fields"];
            coll.drop();

            coll.insert_one({});

            pipeline.add_fields(document{} << "x" << 1 << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                // The server supports add_fields().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].view()["x"].get_int32() == 1);
            } else {
                // The server does not support add_fields().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("bucket") {
            collection coll = db["aggregation_bucket"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 3 << finalize);
            coll.insert_one(document{} << "x" << 5 << finalize);

            pipeline.bucket(document{} << "groupBy"
                                       << "$x"
                                       << "boundaries"
                                       << open_array
                                       << 0
                                       << 2
                                       << 6
                                       << close_array
                                       << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                // The server supports bucket().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 2);

                REQUIRE(results[0].view()["_id"].get_int32() == 0);
                REQUIRE(results[0].view()["count"].get_int32() == 1);

                REQUIRE(results[1].view()["_id"].get_int32() == 2);
                REQUIRE(results[1].view()["count"].get_int32() == 2);
            } else {
                // The server does not support bucket().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("bucket_auto") {
            collection coll = db["aggregation_bucket_auto"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 2 << finalize);
            coll.insert_one(document{} << "x" << 3 << finalize);

            pipeline.bucket_auto(document{} << "groupBy"
                                            << "$x"
                                            << "buckets"
                                            << 2
                                            << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                // The server supports bucket_auto().

                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 2);
                // We check that the "count" field exists here, but we don't assert the exact count,
                // since the server doesn't guarantee what the exact boundaries (and thus the exact
                // counts) will be.
                REQUIRE(results[0].view()["count"]);
                REQUIRE(results[1].view()["count"]);
            } else {
                // The server does not support bucket_auto().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("coll_stats") {
            collection coll = db["aggregation_coll_stats"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << finalize);

            pipeline.coll_stats(document{} << "latencyStats" << open_document << close_document
                                           << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                // The server supports coll_stats().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].view()["ns"]);
                REQUIRE(results[0].view()["latencyStats"]);
            } else {
                // The server does not support coll_stats().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("count") {
            collection coll = db["aggregation_count"];
            coll.drop();

            coll.insert_one({});
            coll.insert_one({});
            coll.insert_one({});

            pipeline.count("foo");
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                // The server supports count().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].view()["foo"].get_int32() == 3);
            } else {
                // The server does not support count().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("facet") {
            collection coll = db["aggregation_facet"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 2 << finalize);
            coll.insert_one(document{} << "x" << 3 << finalize);

            pipeline.facet(document{} << "foo" << open_array << open_document << "$limit" << 2
                                      << close_document
                                      << close_array
                                      << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                // The server supports facet().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 1);
                auto foo_array = results[0].view()["foo"].get_array().value;
                REQUIRE(std::distance(foo_array.begin(), foo_array.end()) == 2);
            } else {
                // The server does not support facet().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("geo_near") {
            collection coll = db["aggregation_geo_near"];
            coll.drop();

            coll.insert_one(document{} << "_id" << 0 << "x" << open_array << 0 << 0 << close_array
                                       << finalize);
            coll.insert_one(document{} << "_id" << 1 << "x" << open_array << 1 << 1 << close_array
                                       << finalize);
            coll.create_index(document{} << "x"
                                         << "2d"
                                         << finalize);

            pipeline.geo_near(document{} << "near" << open_array << 0 << 0 << close_array
                                         << "distanceField"
                                         << "d"
                                         << finalize);
            auto cursor = coll.aggregate(pipeline);

            auto results = get_results(std::move(cursor));
            REQUIRE(results.size() == 2);
            REQUIRE(results[0].view()["d"]);
            REQUIRE(results[0].view()["_id"].get_int32() == 0);
            REQUIRE(results[1].view()["d"]);
            REQUIRE(results[1].view()["_id"].get_int32() == 1);
        }

        SECTION("graph_lookup") {
            collection coll = db["aggregation_graph_lookup"];
            coll.drop();

            coll.insert_one(document{} << "x"
                                       << "bar"
                                       << finalize);
            coll.insert_one(document{} << "x"
                                       << "foo"
                                       << "y"
                                       << "bar"
                                       << finalize);

            pipeline.graph_lookup(document{} << "from" << coll.name() << "startWith"
                                             << "$y"
                                             << "connectFromField"
                                             << "y"
                                             << "connectToField"
                                             << "x"
                                             << "as"
                                             << "z"
                                             << finalize);
            // Add a sort to the pipeline, so below tests can make assumptions about result order.
            pipeline.sort(document{} << "x" << 1 << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                // The server supports graph_lookup().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 2);
                REQUIRE(results[0].view()["z"].get_array().value.empty());
                REQUIRE(!results[1].view()["z"].get_array().value.empty());
            } else {
                // The server does not support graph_lookup().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("group") {
            collection coll = db["aggregation_group"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 2 << finalize);

            pipeline.group(document{} << "_id"
                                      << "$x"
                                      << finalize);
            // Add a sort to the pipeline, so below tests can make assumptions about result order.
            pipeline.sort(document{} << "_id" << 1 << finalize);
            auto cursor = coll.aggregate(pipeline);

            auto results = get_results(std::move(cursor));
            REQUIRE(results.size() == 2);
            REQUIRE(results[0].view()["_id"].get_int32() == 1);
            REQUIRE(results[1].view()["_id"].get_int32() == 2);
        }

        SECTION("index_stats") {
            collection coll = db["aggregation_index_stats"];
            coll.drop();

            coll.create_index(document{} << "a" << 1 << finalize);
            coll.create_index(document{} << "b" << 1 << finalize);
            coll.create_index(document{} << "c" << 1 << finalize);

            pipeline.index_stats();
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 4) {
                // The server supports index_stats().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 4);
            } else {
                // The server does not support index_stats().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("limit") {
            collection coll = db["aggregation_limit"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 2 << finalize);
            coll.insert_one(document{} << "x" << 3 << finalize);

            // Add a sort to the pipeline, so below tests can make assumptions about result order.
            pipeline.sort(document{} << "x" << 1 << finalize);
            pipeline.limit(2);
            auto cursor = coll.aggregate(pipeline);

            auto results = get_results(std::move(cursor));
            REQUIRE(results.size() == 2);
            REQUIRE(results[0].view()["x"].get_int32() == 1);
            REQUIRE(results[1].view()["x"].get_int32() == 2);
        }

        SECTION("lookup") {
            collection coll = db["aggregation_lookup"];
            coll.drop();

            coll.insert_one(document{} << "x" << 0 << finalize);
            coll.insert_one(document{} << "x" << 1 << "y" << 0 << finalize);

            pipeline.lookup(document{} << "from" << coll.name() << "localField"
                                       << "x"
                                       << "foreignField"
                                       << "y"
                                       << "as"
                                       << "z"
                                       << finalize);
            // Add a sort to the pipeline, so below tests can make assumptions about result order.
            pipeline.sort(document{} << "x" << 1 << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 4) {
                // The server supports lookup().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 2);
                REQUIRE(!results[0].view()["z"].get_array().value.empty());
                REQUIRE(results[1].view()["z"].get_array().value.empty());
            } else {
                // The server does not support lookup().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("match") {
            collection coll = db["aggregation_match"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 2 << finalize);

            pipeline.match(document{} << "x" << 1 << finalize);
            auto cursor = coll.aggregate(pipeline);

            auto results = get_results(std::move(cursor));
            REQUIRE(results.size() == 2);
        }

        SECTION("out") {
            collection coll = db["aggregation_out"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << "y" << 1 << finalize);

            pipeline.project(document{} << "x" << 1 << finalize);
            pipeline.out(coll.name().to_string());
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 1) {
                // The server supports out().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.empty());

                auto collection_contents = get_results(coll.find({}));
                REQUIRE(collection_contents.size() == 1);
                REQUIRE(collection_contents[0].view()["x"].get_int32() == 1);
                REQUIRE(!collection_contents[0].view()["y"]);
            } else {
                // The server does not support out().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("project") {
            collection coll = db["aggregation_project"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << "y" << 1 << finalize);

            pipeline.project(document{} << "x" << 1 << finalize);
            auto cursor = coll.aggregate(pipeline);

            auto results = get_results(std::move(cursor));
            REQUIRE(results.size() == 1);
            REQUIRE(results[0].view()["x"].get_int32() == 1);
            REQUIRE(!results[0].view()["y"]);
        }

        SECTION("redact") {
            collection coll = db["aggregation_redact"];
            coll.drop();

            coll.insert_one(
                document{} << "x" << open_document << "secret" << 1 << close_document << "y" << 1
                           << finalize);

            pipeline.redact(document{} << "$cond" << open_document << "if" << open_document << "$eq"
                                       << open_array
                                       << "$secret"
                                       << 1
                                       << close_array
                                       << close_document
                                       << "then"
                                       << "$$PRUNE"
                                       << "else"
                                       << "$$DESCEND"
                                       << close_document
                                       << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 1) {
                // The server supports redact().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 1);
                REQUIRE(!results[0].view()["x"]);
                REQUIRE(results[0].view()["y"].get_int32() == 1);
            } else {
                // The server does not support redact().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("replace_root") {
            collection coll = db["aggregation_replace_root"];
            coll.drop();

            coll.insert_one(document{} << "x" << open_document << "y" << 1 << close_document
                                       << finalize);

            pipeline.replace_root(document{} << "newRoot"
                                             << "$x"
                                             << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                // The server supports replace_root().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].view()["y"]);
            } else {
                // The server does not support replace_root().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("sample") {
            collection coll = db["aggregation_sample"];
            coll.drop();

            coll.insert_one({});
            coll.insert_one({});
            coll.insert_one({});
            coll.insert_one({});

            pipeline.sample(3);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 4) {
                // The server supports sample().
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 3);
            } else {
                // The server does not support sample().
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }

        SECTION("skip") {
            collection coll = db["aggregation_skip"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 2 << finalize);
            coll.insert_one(document{} << "x" << 3 << finalize);

            // Add a sort to the pipeline, so below tests can make assumptions about result order.
            pipeline.sort(document{} << "x" << 1 << finalize);
            pipeline.skip(1);
            auto cursor = coll.aggregate(pipeline);

            auto results = get_results(std::move(cursor));
            REQUIRE(results.size() == 2);
            REQUIRE(results[0].view()["x"].get_int32() == 2);
            REQUIRE(results[1].view()["x"].get_int32() == 3);
        }

        SECTION("sort") {
            collection coll = db["aggregation_sort"];
            coll.drop();

            coll.insert_one(document{} << "x" << 1 << finalize);
            coll.insert_one(document{} << "x" << 2 << finalize);
            coll.insert_one(document{} << "x" << 3 << finalize);

            pipeline.sort(document{} << "x" << -1 << finalize);
            auto cursor = coll.aggregate(pipeline);

            auto results = get_results(std::move(cursor));
            REQUIRE(results.size() == 3);
            REQUIRE(results[0].view()["x"].get_int32() == 3);
            REQUIRE(results[1].view()["x"].get_int32() == 2);
            REQUIRE(results[2].view()["x"].get_int32() == 1);
        }

        SECTION("sort_by_count") {
            insert_many_builder insert_many{options::insert()};
            insert_many(make_document(kvp("x", 1)));
            insert_many(make_document(kvp("x", 2)));
            insert_many(make_document(kvp("x", 2)));

            SECTION("with string") {
                collection coll = db["aggregation_sort_by_count_with_string"];
                coll.drop();

                insert_many.insert(&coll);

                pipeline.sort_by_count("$x");
                auto cursor = coll.aggregate(pipeline);

                if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                    // The server supports sort_by_count().
                    auto results = get_results(std::move(cursor));
                    REQUIRE(results.size() == 2);
                    REQUIRE(results[0].view()["_id"].get_int32() == 2);
                    REQUIRE(results[1].view()["_id"].get_int32() == 1);
                } else {
                    // The server does not support sort_by_count().
                    REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
                }
            }

            SECTION("with document") {
                collection coll = db["aggregation_sort_by_count_with_document"];
                coll.drop();

                insert_many.insert(&coll);

                pipeline.sort_by_count(
                    document{} << "$mod" << open_array << "$x" << 2 << close_array << finalize);
                auto cursor = coll.aggregate(pipeline);

                if (test_util::get_max_wire_version(mongodb_client) >= 5) {
                    // The server supports sort_by_count().
                    auto results = get_results(std::move(cursor));
                    REQUIRE(results.size() == 2);
                    REQUIRE(results[0].view()["_id"].get_int32() == 0);
                    REQUIRE(results[1].view()["_id"].get_int32() == 1);
                } else {
                    // The server does not support sort_by_count().
                    REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
                }
            }
        }

        SECTION("unwind with string") {
            collection coll = db["aggregation_unwind_with_string"];
            coll.drop();

            coll.insert_one(document{} << "x" << open_array << 1 << 2 << 3 << 4 << 5 << close_array
                                       << finalize);
            pipeline.unwind("$x");
            auto cursor = coll.aggregate(pipeline);

            auto results = get_results(std::move(cursor));
            REQUIRE(results.size() == 5);
        }

        SECTION("unwind with document") {
            collection coll = db["aggregation_unwind_with_doc"];
            coll.drop();

            coll.insert_one(document{} << "x" << open_array << 1 << 2 << 3 << 4 << 5 << close_array
                                       << finalize);

            pipeline.unwind(document{} << "path"
                                       << "$x"
                                       << finalize);
            auto cursor = coll.aggregate(pipeline);

            if (test_util::get_max_wire_version(mongodb_client) >= 4) {
                // The server supports unwind() with a document.
                auto results = get_results(std::move(cursor));
                REQUIRE(results.size() == 5);
            } else {
                // The server does not support unwind() with a document.
                REQUIRE_THROWS_AS(get_results(std::move(cursor)), operation_exception);
            }
        }
    }

    SECTION("aggregation with collation", "[collection]") {
        collection coll = db["aggregation_with_collation"];
        coll.drop();

        document b1;
        b1 << "x"
           << "foo";

        coll.insert_one(b1.view());

        auto predicate = document{} << "x"
                                    << "FOO" << finalize;

        pipeline p;
        p.match(predicate.view());

        auto agg_opts = options::aggregate{}.collation(case_insensitive_collation.view());
        auto results = coll.aggregate(p, agg_opts);

        if (test_util::supports_collation(mongodb_client)) {
            REQUIRE(std::distance(results.begin(), results.end()) == 1);
        } else {
            // The server does not support collation.
            REQUIRE_THROWS_AS(std::distance(results.begin(), results.end()), operation_exception);
        }
    }

    SECTION("bulk_write returns correct result object") {
        auto doc1 = document{} << "foo" << 1 << finalize;
        auto doc2 = document{} << "foo" << 2 << finalize;

        options::bulk_write bulk_opts;
        bulk_opts.ordered(false);

        SECTION("default write concern returns result") {
            collection coll = db["bulk_write_default_write"];
            coll.drop();

            bulk_write abulk{bulk_opts};
            abulk.append(model::insert_one{std::move(doc1)});
            abulk.append(model::insert_one{std::move(doc2)});
            auto result = coll.bulk_write(abulk);

            REQUIRE(result);
            REQUIRE(result->inserted_count() == 2);
        }

        SECTION("unacknowledged write concern returns disengaged optional", "[collection]") {
            collection coll = db["bulk_write_unack_write"];
            coll.drop();

            bulk_opts.write_concern(noack);
            bulk_write bbulk{bulk_opts};
            bbulk.append(model::insert_one{std::move(doc1)});
            bbulk.append(model::insert_one{std::move(doc2)});
            auto result = coll.bulk_write(bbulk);

            REQUIRE(!result);

            // Block until server has received the write request, to prevent
            // this unacknowledged write from racing with writes to this
            // collection from other sections.
            db.run_command(document{} << "getLastError" << 1 << finalize);
        }

        SECTION("write wrapper returns correct result") {
            collection coll = db["bulk_write_write_wrapper"];
            coll.drop();

            auto doc3 = make_document(kvp("foo", 3));
            auto result = coll.write(model::insert_one{std::move(doc3)});
            REQUIRE(result);
            REQUIRE(result->inserted_count() == 1);
        }
    }

    SECTION("distinct works", "[collection]") {
        collection coll = db["distinct"];
        coll.drop();
        auto doc1 = document{} << "foo"
                               << "baz"
                               << "garply" << 1 << finalize;
        auto doc2 = document{} << "foo"
                               << "bar"
                               << "garply" << 2 << finalize;
        auto doc3 = document{} << "foo"
                               << "baz"
                               << "garply" << 2 << finalize;
        auto doc4 = document{} << "foo"
                               << "quux"
                               << "garply" << 9 << finalize;

        options::bulk_write bulk_opts;
        bulk_opts.ordered(false);
        bulk_write bulk{bulk_opts};

        bulk.append(model::insert_one{std::move(doc1)});
        bulk.append(model::insert_one{std::move(doc2)});
        bulk.append(model::insert_one{std::move(doc3)});
        bulk.append(model::insert_one{std::move(doc4)});

        coll.bulk_write(bulk);

        REQUIRE(coll.count({}) == 4);

        auto distinct_results = coll.distinct("foo", {});

        // copy into a vector.
        std::vector<bsoncxx::document::value> results;
        for (auto&& result : distinct_results) {
            results.emplace_back(result);
        }

        REQUIRE(results.size() == std::size_t{1});

        auto res_doc = results[0].view();
        auto values_array = res_doc["values"].get_array().value;

        std::vector<stdx::string_view> distinct_values;
        for (auto&& value : values_array) {
            distinct_values.push_back(value.get_utf8().value);
        }

        const auto assert_contains_one = [&](stdx::string_view val) {
            REQUIRE(std::count(distinct_values.begin(), distinct_values.end(), val) == 1);
        };

        assert_contains_one("baz");
        assert_contains_one("bar");
        assert_contains_one("quux");
    }

    SECTION("distinct with collation", "[collection]") {
        collection coll = db["distinct_with_collation"];
        coll.drop();
        auto doc = document{} << "x"
                              << "foo" << finalize;

        coll.insert_one(doc.view());

        auto predicate = document{} << "x"
                                    << "FOO" << finalize;

        auto distinct_opts = options::distinct{}.collation(case_insensitive_collation.view());

        if (test_util::supports_collation(mongodb_client)) {
            auto distinct_results = coll.distinct("x", predicate.view(), distinct_opts);
            auto iter = distinct_results.begin();
            REQUIRE(iter != distinct_results.end());
            auto result = *iter;
            auto values = result["values"].get_array().value;
            REQUIRE(std::distance(values.begin(), values.end()) == 1);
            REQUIRE(values[0].get_utf8().value == stdx::string_view{"foo"});
        } else {
            // The server does not support collation.
            REQUIRE_THROWS_AS(coll.distinct("x", predicate.view(), distinct_opts),
                              operation_exception);
        }
    }
}

TEST_CASE("read_concern is inherited from parent", "[collection]") {
    client mongo_client{uri{}};
    database db = mongo_client["collection_read_concern_inheritance"];

    read_concern::level majority = read_concern::level::k_majority;
    read_concern::level local = read_concern::level::k_local;

    read_concern rc{};
    rc.acknowledge_level(majority);
    db.read_concern(rc);

    SECTION("when parent is a database") {
        collection coll = db["database_parent"];
        REQUIRE(coll.read_concern().acknowledge_level() == read_concern::level::k_majority);
    }

    SECTION("except when read_concern is explicitly set") {
        collection coll = db["explicitly_set"];
        read_concern set_rc{};
        set_rc.acknowledge_level(read_concern::level::k_local);
        coll.read_concern(set_rc);

        REQUIRE(coll.read_concern().acknowledge_level() == local);
    }
}

void find_index_and_validate(collection& coll,
                             stdx::string_view index_name,
                             const std::function<void(bsoncxx::document::view)>& validate =
                                 [](bsoncxx::document::view) {}) {
    auto cursor = coll.list_indexes();

    for (auto&& index : cursor) {
        auto name_ele = index["name"];
        REQUIRE(name_ele);
        REQUIRE(name_ele.type() == bsoncxx::type::k_utf8);

        if (name_ele.get_utf8().value != index_name) {
            continue;
        }

        validate(index);
        return;
    }
    REQUIRE(false);  // index of given name not found
}

TEST_CASE("create_index tests", "[collection]") {
    using namespace bsoncxx;

    instance::current();

    client mongodb_client{uri{}};
    database db = mongodb_client["collection_create_index"];

    SECTION("returns index name") {
        collection coll = db["create_index_return_name"];
        coll.drop();
        coll.insert_one({});  // Ensure that the collection exists.

        bsoncxx::document::value index = make_document(kvp("a", 1));

        std::string indexName{"myName"};
        options::index options{};
        options.name(indexName);

        auto response = coll.create_index(index.view(), options);
        REQUIRE(response.view()["name"].get_utf8().value == bsoncxx::stdx::string_view{indexName});

        find_index_and_validate(coll, indexName);

        bsoncxx::document::value index2 = make_document(kvp("b", 1), kvp("c", -1));

        auto response2 = coll.create_index(index2.view(), options::index{});
        REQUIRE(response2.view()["name"].get_utf8().value ==
                bsoncxx::stdx::string_view{"b_1_c_-1"});

        find_index_and_validate(coll, "b_1_c_-1");
    }

    SECTION("with collation") {
        collection coll = db["create_index_with_collation"];
        coll.drop();
        coll.insert_one({});  // Ensure that the collection exists.

        bsoncxx::document::value keys = make_document(kvp("a", 1));
        auto collation = make_document(kvp("locale", "en_US"));

        options::index options{};
        options.collation(collation.view());

        coll.create_index(keys.view(), options);

        auto validate = [](bsoncxx::document::view index) {
            bsoncxx::types::value locale{types::b_utf8{"en_US"}};
            auto locale_ele = index["collation"]["locale"];
            REQUIRE(locale_ele);
            REQUIRE(locale_ele.type() == type::k_utf8);
            REQUIRE((locale_ele.get_utf8() == locale));
        };

        find_index_and_validate(coll, "a_1", validate);
    }

    SECTION("fails") {
        collection coll = db["create_index_fails"];
        coll.drop();
        coll.insert_one({});  // Ensure that the collection exists.

        bsoncxx::document::value keys1 = make_document(kvp("a", 1));
        bsoncxx::document::value keys2 = make_document(kvp("a", -1));

        options::index options{};
        options.name("a");

        REQUIRE_NOTHROW(coll.create_index(keys1.view(), options));
        REQUIRE_THROWS_AS(coll.create_index(keys2.view(), options), operation_exception);
    }

    SECTION("succeeds with options") {
        collection coll = db["create_index_with_options"];
        coll.drop();
        coll.insert_one({});  // Ensure that the collection exists.

        mongocxx::stdx::string_view index_name{"succeeds_with_options"};

        bsoncxx::document::value keys = make_document(kvp("cccc", 1));

        options::index options{};
        options.unique(true);
        options.expire_after(std::chrono::seconds(500));
        options.name(index_name);

        REQUIRE_NOTHROW(coll.create_index(keys.view(), options));

        bool unique = options.unique().value();
        auto validate = [unique](bsoncxx::document::view index) {
            auto expire_after = index["expireAfter"];
            REQUIRE(expire_after);
            REQUIRE(expire_after.type() == type::k_int32);
            REQUIRE(expire_after.get_int32().value == 500);

            auto unique_ele = index["unique"];
            REQUIRE(unique_ele);
            REQUIRE(unique_ele.type() == type::k_bool);
            REQUIRE(unique_ele.get_bool() == unique);
        };

        find_index_and_validate(coll, index_name, validate);
    }

    SECTION("fails with options") {
        collection coll = db["create_index_fails_with_options"];
        coll.drop();
        coll.insert_one({});  // Ensure that the collection exists.

        bsoncxx::document::value keys = make_document(kvp("c", 1));
        options::index options{};

        auto expire_after =
            std::chrono::seconds(static_cast<int64_t>(std::numeric_limits<int32_t>::max()) + 1);
        options.expire_after(expire_after);
        REQUIRE_THROWS_AS(coll.create_index(keys.view(), options), logic_error);

        expire_after = std::chrono::seconds(-1);
        options.expire_after(expire_after);
        REQUIRE_THROWS_AS(coll.create_index(keys.view(), options), logic_error);
    }

    SECTION("succeeds with storage engine options") {
        collection coll = db["create_index_succeeds_with_storage_options"];
        coll.drop();
        coll.insert_one({});  // Ensure that the collection exists.

        bsoncxx::stdx::string_view index_name{"storage_options_test"};
        bsoncxx::document::value keys = make_document(kvp("c", 1));

        options::index options{};
        options.name(index_name);

        std::unique_ptr<options::index::wiredtiger_storage_options> wt_options =
            bsoncxx::stdx::make_unique<options::index::wiredtiger_storage_options>();
        wt_options->config_string("block_allocation=first");

        REQUIRE_NOTHROW(options.storage_options(std::move(wt_options)));
        REQUIRE_NOTHROW(coll.create_index(keys.view(), options));

        auto validate = [](bsoncxx::document::view index) {
            auto config_string_ele = index["storageEngine"]["wiredTiger"]["configString"];
            REQUIRE(config_string_ele);
            REQUIRE(config_string_ele.type() == type::k_utf8);
            REQUIRE(config_string_ele.get_utf8() == types::b_utf8{"block_allocation=first"});
        };

        find_index_and_validate(coll, index_name, validate);
    }
}

// We use a capped collection for this test case so we can
// use it with all three cursor types.
TEST_CASE("Cursor iteration", "[collection][cursor]") {
    instance::current();
    client mongodb_client{uri{}};
    database db = mongodb_client["collection_cursor_iteration"];

    auto capped_name = std::string("mongo_cxx_driver_capped");
    collection coll = db[capped_name];

    // Drop and (re)create the capped collection.
    coll.drop();
    auto create_opts = options::create_collection{}.capped(true).size(1024 * 1024);
    db.create_collection(capped_name, create_opts);

    // Tests will use all three cursor types.
    options::find opts;
    std::string type_str;

    SECTION("k_non_tailable") {
        opts.cursor_type(cursor::type::k_non_tailable);
        type_str = "k_non_tailable";
    }

    SECTION("k_tailable") {
        opts.cursor_type(cursor::type::k_tailable);
        type_str = "k_tailable";
    }

    SECTION("k_tailable_await") {
        opts.cursor_type(cursor::type::k_tailable_await);
        type_str = "k_tailable_await";

        // Improve execution time by reducing the amount of time the server waits for new results
        // for this cursor.
        opts.max_await_time(std::chrono::milliseconds{1});
    }

    INFO(type_str);

    // Insert 3 documents.
    for (int32_t n : {1, 2, 3}) {
        coll.insert_one(make_document(kvp("x", n)));
    }

    auto cursor = coll.find({}, opts);
    auto iter = cursor.begin();

    REQUIRE(iter == cursor.begin());

    // Check that the cursor finds three documents and that the iterator
    // stays in lockstep.
    auto expected = 1;

    for (auto&& doc : cursor) {
        REQUIRE(doc["x"].get_int32() == expected);

        // Lockstep requires that iter matches both the current document
        // and cursor.begin() (current doc before cursor increment).
        // It must not match cursor.end(), since a document exists.
        REQUIRE(iter == cursor.begin());
        REQUIRE(iter != cursor.end());
        REQUIRE((*iter)["x"].get_int32() == expected);

        expected++;
    }

    // Check that iteration covered all three documents.
    REQUIRE(expected == 4);

    // As no document is available, iterator now must match cursor.end().
    // We check both LHS and RHS for coverage.
    REQUIRE(iter == cursor.end());
    REQUIRE(cursor.end() == iter);

    // Because there are no more documents available from this query,
    // cursor.begin() must equal cursor.end().  Transitively, this means
    // that iter must also match cursor.begin().
    REQUIRE(cursor.begin() == cursor.end());
    REQUIRE(iter == cursor.begin());

    // For tailable cursors, if more documents are inserted, the next
    // call to cursor.begin() should find more documents and the existing iterator
    // should no longer be exhausted.
    if (opts.cursor_type() != cursor::type::k_non_tailable) {
        // Insert 3 more documents.
        for (int32_t n : {4, 5, 6}) {
            coll.insert_one(make_document(kvp("x", n)));
        }

        // More documents are available, but until the next call to
        // cursor.begin(), the existing iterator still appears exhausted.
        REQUIRE(iter == cursor.end());

        // After calling cursor.begin(), the existing iterator is revived.
        cursor.begin();
        REQUIRE(iter != cursor.end());
        REQUIRE(iter == cursor.begin());

        // Check that the cursor finds the next three documents and that the
        // iterator stays in lockstep.
        for (auto&& doc : cursor) {
            REQUIRE(doc["x"].get_int32() == expected);

            REQUIRE(iter == cursor.begin());
            REQUIRE(iter != cursor.end());
            REQUIRE((*iter)["x"].get_int32() == expected);

            expected++;
        }

        // Check that iteration has covered all six documents.
        REQUIRE(expected == 7);

        // As before: iter, cursor.begin() and cursor.end() must all
        // transitively agree that the cursor is currently exhausted.
        REQUIRE(iter == cursor.end());
        REQUIRE(cursor.begin() == cursor.end());
        REQUIRE(iter == cursor.begin());
    }
}

TEST_CASE("regressions", "CXX-986") {
    instance::current();
    mongocxx::uri mongo_uri{"mongodb://non-existent-host.invalid/"};
    mongocxx::client client{mongo_uri};
    REQUIRE_THROWS(client.database("irrelevant")["irrelevant"].find_one_and_update(
        document{} << "irrelevant" << 1 << finalize, document{} << "irrelevant" << 2 << finalize));
}
}  // namespace
