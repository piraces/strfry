#pragma once

#include "golpe.h"

#include "constants.h"


struct FilterSetBytes {
    struct Item {
        uint16_t offset;
        uint8_t size;
        uint8_t firstByte;
    };

    std::vector<Item> items;
    std::string buf;

    // Sizes are post-hex decode 

    FilterSetBytes(const tao::json::value &arrHex, bool hexDecode, size_t minSize, size_t maxSize) {
        std::vector<std::string> arr;

        uint64_t totalSize = 0;

        for (const auto &i : arrHex.get_array()) {
            arr.emplace_back(hexDecode ? from_hex(i.get_string(), false) : i.get_string());
            size_t itemSize = arr.back().size();
            if (itemSize < minSize) throw herr("filter item too small");
            if (itemSize > maxSize) throw herr("filter item too large");
            totalSize += itemSize;
        }

        std::sort(arr.begin(), arr.end());

        for (const auto &item : arr) {
            if (items.size() > 0 && item.starts_with(at(items.size() - 1))) continue; // remove duplicates and redundant prefixes
            items.emplace_back(Item{ (uint16_t)buf.size(), (uint8_t)item.size(), (uint8_t)item[0] });
            buf += item;
        }

        if (buf.size() > 65535) throw herr("total filter items too large");
    }

    std::string at(size_t n) const {
        if (n >= items.size()) throw("FilterSetBytes access out of bounds");
        auto &item = items[n];
        return buf.substr(item.offset, item.size);
    }

    size_t size() const {
        return items.size();
    }

    bool doesMatch(std::string_view candidate) const {
        if (candidate.size() == 0) throw herr("invalid candidate");

        // Binary search for upper-bound: https://en.cppreference.com/w/cpp/algorithm/upper_bound

        ssize_t first = 0, last = items.size(), curr;
        ssize_t count = last - first, step;

        while (count > 0) {
            curr = first; 
            step = count / 2;
            curr += step;

            bool comp = (uint8_t)candidate[0] != items[curr].firstByte
                        ? (uint8_t)candidate[0] < items[curr].firstByte
                        : candidate < std::string_view(buf.data() + items[curr].offset, items[curr].size);
     
            if (!comp) {
                first = ++curr;
                count -= step + 1;
            } else {
                count = step;
            }
        }

        if (first == 0) return false;
        if (candidate.starts_with(std::string_view(buf.data() + items[first - 1].offset, items[first - 1].size))) return true;

        return false;
    }
};

struct FilterSetUint {
    std::vector<uint64_t> items;

    FilterSetUint(const tao::json::value &arr) {
        for (const auto &i : arr.get_array()) {
            items.push_back(i.get_unsigned());
        }

        std::sort(items.begin(), items.end());

        items.erase(std::unique(items.begin(), items.end()), items.end()); // remove duplicates
    }

    uint64_t at(size_t n) const {
        if (n >= items.size()) throw("FilterSetBytes access out of bounds");
        return items[n];
    }

    size_t size() const {
        return items.size();
    }

    bool doesMatch(uint64_t candidate) const {
        return std::binary_search(items.begin(), items.end(), candidate);
    }
};

struct NostrFilter {
    std::optional<FilterSetBytes> ids;
    std::optional<FilterSetBytes> authors;
    std::optional<FilterSetUint> kinds;
    std::map<char, FilterSetBytes> tags;

    uint64_t since = 0;
    uint64_t until = MAX_U64;
    uint64_t limit = MAX_U64;
    bool neverMatch = false;
    bool indexOnlyScans = false;

    explicit NostrFilter(const tao::json::value &filterObj) {
        uint64_t numMajorFields = 0;

        for (const auto &[k, v] : filterObj.get_object()) {
            if (v.is_array() && v.get_array().size() == 0) {
                neverMatch = true;
                break;
            }

            if (k == "ids") {
                ids.emplace(v, true, 1, 32);
                numMajorFields++;
            } else if (k == "authors") {
                authors.emplace(v, true, 1, 32);
                numMajorFields++;
            } else if (k == "kinds") {
                kinds.emplace(v);
                numMajorFields++;
            } else if (k.starts_with('#')) {
                numMajorFields++;
                if (k.size() == 2) {
                    char tag = k[1];

                    if (tag == 'p' || tag == 'e') {
                        tags.emplace(tag, FilterSetBytes(v, true, 32, 32));
                    } else {
                        tags.emplace(tag, FilterSetBytes(v, false, 1, cfg().events__maxTagValSize));
                    }
                } else {
                    throw herr("unindexed tag filter");
                }
            } else if (k == "since") {
                since = v.get_unsigned();
            } else if (k == "until") {
                until = v.get_unsigned();
            } else if (k == "limit") {
                limit = v.get_unsigned();
            } else {
                throw herr("unrecognised filter item");
            }
        }

        if (tags.size() > 2) throw herr("too many tags in filter"); // O(N^2) in matching, just prohibit it

        if (limit > cfg().relay__maxFilterLimit) limit = cfg().relay__maxFilterLimit;

        indexOnlyScans = numMajorFields <= 1;
        // FIXME: pubkeyKind scan could be serviced index-only too
    }

    bool doesMatchTimes(uint64_t created) const {
        if (created < since) return false;
        if (created > until) return false;
        return true;
    }

    bool doesMatch(const NostrIndex::Event *ev) const {
        if (neverMatch) return false;

        if (!doesMatchTimes(ev->created_at())) return false;

        if (ids && !ids->doesMatch(sv(ev->id()))) return false;
        if (authors && !authors->doesMatch(sv(ev->pubkey()))) return false;
        if (kinds && !kinds->doesMatch(ev->kind())) return false;

        for (const auto &[tag, filt] : tags) {
            bool foundMatch = false;

            for (const auto &tagPair : *(ev->tags())) {
                auto eventTag = tagPair->key();
                if (eventTag == tag && filt.doesMatch(sv(tagPair->val()))) {
                    foundMatch = true;
                    break;
                }
            }

            if (!foundMatch) return false;
        }

        return true;
    }
};

struct NostrFilterGroup {
    std::vector<NostrFilter> filters;

    // Note that this expects the full array, so the first two items are "REQ" and the subId
    NostrFilterGroup(const tao::json::value &req) {
        const auto &arr = req.get_array();
        if (arr.size() < 3) throw herr("too small");

        for (size_t i = 2; i < arr.size(); i++) {
            filters.emplace_back(arr[i]);
            if (filters.back().neverMatch) filters.pop_back();
        }
    }

    // Hacky! Deserves a refactor
    static NostrFilterGroup unwrapped(tao::json::value filter) {
        if (!filter.is_array()) {
            filter = tao::json::value::array({ filter });
        }

        tao::json::value pretendReqQuery = tao::json::value::array({ "REQ", "junkSub" });

        for (auto &e : filter.get_array()) {
            pretendReqQuery.push_back(e);
        }

        return NostrFilterGroup(pretendReqQuery);
    }

    bool doesMatch(const NostrIndex::Event *ev) const {
        for (const auto &f : filters) {
            if (f.doesMatch(ev)) return true;
        }

        return false;
    }

    size_t size() const {
        return filters.size();
    }
};
