#pragma once

#include <string>
#include <vector>

#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view_or_value.hpp>
#include <bsoncxx/stdx/optional.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/search_index_view.hpp>
#include <mongocxx/search_index_model.hpp>

#include <mongocxx/config/prelude.hpp>

namespace mongocxx {
MONGOCXX_INLINE_NAMESPACE_BEGIN

///
/// Class representing a MongoDB search index view.
///
class MONGOCXX_API search_index_view {
   public:
    search_index_view(search_index_view&&) noexcept;
    search_index_view& operator=(search_index_view&&) noexcept;

    ~search_index_view();

    ///
    /// @{
    ///
    /// Returns a cursor over all the search indexes.
    ///
    cursor list(const bsoncxx::document::view& aggregation_opts = bsoncxx::document::view{});

    cursor list(const std::string name,
                const bsoncxx::document::view& aggregation_opts = bsoncxx::document::view{});

    /**
     * This is a convenience method for creating a single search index.
     *
     * @param name
     *    The name of the search index to create.
     * @param definition
     *    The document describing the search index to be created.
     *
     * @return The name of the created search index.
     *
     */
    bsoncxx::stdx::optional<std::string> create_one(
        const std::string name,
        const bsoncxx::document::view_or_value& definition,
        const options::search_index_view& options = options::search_index_view{});

    /**
     * This is a convenience method for creating a single search index.
     *
     * @param model
     *    The search index model to create.
     *
     * @return The name of the created index.
     *
     */
    bsoncxx::stdx::optional<std::string> create_one(
        const search_index_model& model,
        const options::search_index_view& options = options::search_index_view{});

    /**
     * Creates multiple search indexes in the collection.
     *
     * @param models
     *    The search index models to create.
     *
     * @return The names of the created indexes.
     *
     */
    std::vector<std::string> create_many(
        const std::vector<search_index_model>& models,
        const options::search_index_view& options = options::search_index_view{});

    /**
     * Drops a single search index from the collection by the index name.
     *
     * @param name
     *    The name of the search index to drop.
     *
     */
    void drop_one(const std::string name,
                  const options::search_index_view& options = options::search_index_view{});

    /**
     * Updates a single search index from the collection by the search index name.
     *
     *
     * @param name
     *    The name of the search index to update.
     *
     * @param definition
     *    The definition to update the search index to.
     *
     */
    void update_one(std::string name,
                    const bsoncxx::document::view_or_value& definition,
                    const options::search_index_view& options = options::search_index_view{});

   private:
    friend class collection;
    class MONGOCXX_PRIVATE impl;

    MONGOCXX_PRIVATE search_index_view(void* coll, void* client);

    MONGOCXX_PRIVATE impl& _get_impl();

   private:
    std::unique_ptr<impl> _impl;
};

MONGOCXX_INLINE_NAMESPACE_END
}  // namespace mongocxx

#include <mongocxx/config/postlude.hpp>
