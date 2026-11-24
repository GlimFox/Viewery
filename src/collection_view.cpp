#include "collection_view.hpp"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cwchar>
#include <map>
#include <cwctype>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

namespace viewery {
namespace {

std::wstring folded(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = static_cast<int>(value.size());
    const int size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
        value.data(), length, nullptr, 0, nullptr, nullptr, 0);
    if (!size) return std::wstring(value);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE,
        value.data(), length, result.data(), size, nullptr, nullptr, 0);
    return result;
}

std::wstring pathKey(const std::filesystem::path& path)
{
    return folded(path.lexically_normal().native());
}

bool contains(std::wstring_view haystack, const std::wstring& needle)
{
    return needle.empty() || folded(haystack).find(needle) != std::wstring::npos;
}

int natural(const std::wstring& a, const std::wstring& b)
{
    return StrCmpLogicalW(a.c_str(), b.c_str());
}

std::wstring branch(const std::wstring& parent, std::wstring_view type, const std::wstring& id)
{
    return parent + L"/" + std::wstring(type) + L":" + std::to_wstring(id.size()) + L":" + id;
}

std::wstring displayedTitle(const Artwork* work)
{
    return work ? (work->userTitle.empty() ? work->title : work->userTitle) : std::wstring{};
}

struct Item {
    const MediaRecord* file = nullptr;
    const Artwork* work = nullptr;
    const MediaVersion* version = nullptr;
    std::vector<const Artist*> artists;
    std::vector<std::shared_ptr<Artist>> inferredArtists;
    std::filesystem::path path;
    std::wstring name, extension, title, authorNames;
    std::size_t ordinal = 0;
    std::int64_t order = 0;
};

std::wstring trimName(std::wstring name)
{
    const auto start=name.find_first_not_of(L" _-\t");
    if(start==std::wstring::npos) return {};
    return name.substr(start,name.find_last_not_of(L" _-\t")-start+1);
}

std::wstring artistFromName(const std::filesystem::path& path)
{
    const auto stem=path.stem().wstring();
    std::wstring name;
    if(stem.starts_with(L"[")) {
        const auto end=stem.find(L']',1);
        if(end!=std::wstring::npos) name=stem.substr(1,end-1);
    }
    if(name.empty()) {
        for(std::size_t at=1;at+1<stem.size();++at) {
            if(stem[at]!=L'_'&&stem[at]!=L'-') continue;
            auto end=at+1;
            while(end<stem.size()&&stem[end]>=L'0'&&stem[end]<=L'9') ++end;
            const auto digits=end-at-1;
            if(stem[at]==L'_'&&digits>=1&&digits<=9&&end+1<stem.size()&&stem[end]==L'_') {
                name=stem.substr(0,at);break;
            }
            // Social-export names have two long numeric IDs. A random hash
            // ending in "-7" is not evidence of an artist.
            if(stem[at]==L'-'&&digits>=6&&end+1<stem.size()&&stem[end]==L'-') {
                auto second=end+1;
                while(second<stem.size()&&stem[second]>=L'0'&&stem[second]<=L'9') ++second;
                if(second-end-1>=6&&(second==stem.size()||stem[second]==L'_'||stem[second]==L'-')) {
                    name=stem.substr(0,at);break;
                }
            }
        }
    }
    if(name.empty()) {
        const auto at=stem.find(L" - ");
        if(at!=std::wstring::npos) name=stem.substr(0,at);
    }
    name=trimName(std::move(name));
    if(name.size()>120||name.empty()||!std::ranges::any_of(name,[](wchar_t ch){
        WORD type=0;return GetStringTypeW(CT_CTYPE1,&ch,1,&type)&&(type&C1_ALPHA)!=0;
    })) return {};
    const auto key=folded(name);
    static const std::set<std::wstring> generic{L"IMAGE",L"IMG",L"PHOTO",L"PICTURE",L"SCREENSHOT",L"SCREEN",L"FRAME",
        L"CAPTURE",L"VIDEO",L"VID",L"PXL",L"DSC",L"PIC",L"CANVAS",L"UNTITLED",L"DOWNLOAD",L"ФОТО",L"СКРИНШОТ"};
    if(generic.contains(key)||key.starts_with(L"CODEX-CLIPBOARD")) return {};
    for(const auto& prefix:generic) if(key.starts_with(prefix)&&key.size()>prefix.size()&&
        std::ranges::all_of(std::wstring_view(key).substr(prefix.size()),[](wchar_t ch){return ch>=L'0'&&ch<=L'9';})) return {};
    return name;
}

bool artistBoundary(wchar_t character)
{
    return std::wstring_view(L" _-&+[]()\t").find(character)!=std::wstring_view::npos;
}

struct ArtistNames {
    std::map<std::wstring,const Artist*> tokens;
    std::set<std::wstring> canonical;

    void add(std::wstring name,const Artist& artist,bool primary)
    {
        const auto token=folded(trimName(std::move(name)));
        if(token.empty()) return;
        if(!primary&&canonical.contains(token)) return;
        const auto [entry,inserted]=tokens.emplace(token,&artist);
        if(!inserted&&(!entry->second||entry->second->id!=artist.id)) entry->second=nullptr;
        if(primary) canonical.insert(token);
    }

    explicit ArtistNames(const std::vector<Artist>& artists)
    {
        for(const auto& artist:artists) {add(artist.name,artist,true);add(artist.fileName,artist,true);}
        for(const auto& artist:artists) {
            std::size_t start=0;
            do {
                const auto end=artist.aliases.find_first_of(L",;\r\n",start);
                add(artist.aliases.substr(start,end==std::wstring::npos?end:end-start),artist,false);
                if(end==std::wstring::npos) break;
                start=end+1;
            } while(start<artist.aliases.size());
        }
    }

    const Artist* exact(std::wstring_view name) const
    {
        const auto entry=tokens.find(folded(name));
        return entry==tokens.end()?nullptr:entry->second;
    }

    std::vector<const Artist*> matching(std::wstring_view name) const
    {
        const auto haystack=folded(name);
        std::vector<const Artist*> found;
        std::set<LibraryId> ids;
        for(std::size_t start=0;start<haystack.size();++start) {
            if(start!=0&&!artistBoundary(haystack[start-1])) continue;
            for(std::size_t end=haystack.size();end>start;--end) {
                if(end!=haystack.size()&&!artistBoundary(haystack[end])) continue;
                const auto entry=tokens.find(haystack.substr(start,end-start));
                if(entry==tokens.end()||!entry->second) continue;
                if(ids.insert(entry->second->id).second) found.push_back(entry->second);
                start=end-1;
                break;
            }
        }
        return found;
    }
};

void filenameArtists(Item& item,const ArtistNames& names)
{
    std::set<LibraryId> ids;
    const auto add=[&](const Artist* artist) {
        if(!artist||!ids.insert(artist->id).second) return;
        item.artists.push_back(artist);
        if(!item.authorNames.empty()) item.authorNames+=L" & ";
        item.authorNames+=artist->name;
    };
    const auto prefix=artistFromName(item.path);
    if(prefix.empty()) {
        for(const auto* artist:names.matching(item.path.stem().wstring())) add(artist);
        return;
    }
    if(const auto* artist=names.exact(prefix)) {add(artist);return;}
    if(names.matching(prefix).empty()) {
        const auto anywhere=names.matching(item.path.stem().wstring());
        if(!anywhere.empty()) {for(const auto* artist:anywhere) add(artist);return;}
    }
    std::size_t start=0;
    do {
        const auto end=prefix.find_first_of(L"&+",start);
        const auto author=trimName(prefix.substr(start,end==std::wstring::npos?end:end-start));
        if(!author.empty()) {
            if(const auto* artist=names.exact(author)) add(artist);
            else {
                const auto known=names.matching(author);
                if(!known.empty()) for(const auto* registered:known) add(registered);
                else {
                    auto inferred=std::make_shared<Artist>();
                    inferred->id=L"filename:"+folded(author);inferred->name=author;
                    add(inferred.get());item.inferredArtists.push_back(std::move(inferred));
                }
            }
        }
        if(end==std::wstring::npos) break;
        start=end+1;
    } while(start<prefix.size());
}

std::int64_t customOrder(const MediaRecord* file, std::size_t ordinal)
{
    if (file) {
        const auto found = file->fields.find(L"order");
        if (found != file->fields.end() && !found->second.empty() &&
            found->second.find_first_not_of(L"-0123456789") == std::wstring::npos) {
            wchar_t* end = nullptr;
            const auto value = std::wcstoll(found->second.c_str(), &end, 10);
            if (end == found->second.c_str() + found->second.size()) return value;
        }
    }
    return static_cast<std::int64_t>(ordinal);
}

template<class Record>
auto recordIndex(const std::vector<Record>& records)
{
    std::map<LibraryId, const Record*> index;
    for (const auto& record : records) index.emplace(record.id, &record);
    return index;
}

template<class Record>
const Record* findRecord(const std::map<LibraryId, const Record*>& index, const LibraryId& id)
{
    const auto found = index.find(id);
    return found == index.end() ? nullptr : found->second;
}

bool matchesVersion(const Item& item, const std::wstring& filter)
{
    if (filter.empty()) return true;
    if (!item.version) return false;
    std::wstring numeric = filter;
    if (numeric.front() == L'V') numeric.erase(0, 1);
    if (!numeric.empty() && numeric.find_first_not_of(L"0123456789") == std::wstring::npos) {
        wchar_t* end = nullptr;
        const auto number = std::wcstoll(numeric.c_str(), &end, 10);
        return end == numeric.c_str() + numeric.size() && item.version->number == number;
    }
    return contains(item.version->title, filter);
}

bool matches(const Item& item, const CollectionFilter& filter)
{
    if (filter.unassigned && !item.artists.empty()) return false;
    if (filter.untitled && !item.title.empty()) return false;
    if (filter.unorganized && item.work && item.version && !item.artists.empty()) return false;
    if (!filter.artist.empty()) {
        bool artistMatch = false;
        for (const auto* artist : item.artists) {
            artistMatch |= contains(artist->name, filter.artist) || contains(artist->fileName, filter.artist) ||
                           contains(artist->aliases, filter.artist);
        }
        if (!artistMatch) return false;
    }
    if (!contains(item.title, filter.title) || !matchesVersion(item, filter.version)) return false;
    if (!filter.format.empty()) {
        auto format = filter.format;
        if (format.front() != L'.') format.insert(format.begin(), L'.');
        if (folded(item.extension) != format) return false;
    }
    if (filter.text.empty()) return true;
    return contains(item.name, filter.text) || contains(item.title, filter.text) ||
           contains(item.authorNames, filter.text) ||
           (item.version && contains(item.version->title, filter.text));
}

std::vector<Item> matchingItems(const CollectionSnapshot& collection,
                                const std::vector<std::filesystem::path>& paths,
                                const CollectionViewOptions& options)
{
    std::map<std::wstring, const MediaRecord*> files;
    for (const auto& file : collection.files) files.emplace(pathKey(file.path), &file);
    const auto artists = recordIndex(collection.artists);
    const auto works = recordIndex(collection.artworks);
    const auto versions = recordIndex(collection.versions);
    const ArtistNames artistNames(collection.artists);
    const bool inferNames=options.filenameArtists||options.group==CollectionGroup::artist||options.sort==CollectionSort::artist||!options.filter.artist.empty();
    auto filter = options.filter;
    filter.text = folded(filter.text);
    filter.artist = folded(filter.artist);
    filter.title = folded(filter.title);
    filter.version = folded(filter.version);
    filter.format = folded(filter.format);
    std::set<std::wstring> seen;
    std::vector<Item> result;
    result.reserve(paths.size());
    for (std::size_t ordinal = 0; ordinal < paths.size(); ++ordinal) {
        const auto key = pathKey(paths[ordinal]);
        if (!seen.insert(key).second) continue;
        Item item;
        item.path = paths[ordinal];
        item.name = item.path.filename().wstring();
        item.extension = item.path.extension().wstring();
        item.ordinal = ordinal;
        if (const auto file = files.find(key); file != files.end()) {
            item.file = file->second;
            item.work = findRecord(works, item.file->artworkId);
            item.version = findRecord(versions, item.file->versionId);
            if (item.version && (!item.work || item.version->artworkId != item.work->id)) item.version = nullptr;
            if (item.work) {
                std::set<LibraryId> authorIds;
                for (const auto& id : item.work->artists) {
                    const auto* artist = findRecord(artists, id);
                    if (!artist || !authorIds.insert(id).second) continue;
                    item.artists.push_back(artist);
                    if (!item.authorNames.empty()) item.authorNames += L" & ";
                    item.authorNames += artist->name;
                }
                item.title = displayedTitle(item.work);
            }
        }
        if(options.filenameArtists) {item.artists.clear();item.authorNames.clear();}
        if(inferNames&&item.artists.empty()) {
            filenameArtists(item,artistNames);
        }
        item.order = customOrder(item.file, ordinal);
        if (matches(item, filter)) result.push_back(std::move(item));
    }
    return result;
}

template<class Value>
int compare(Value a, Value b)
{
    return a < b ? -1 : a > b ? 1 : 0;
}

int compareItems(const Item& a, const Item& b, CollectionSort sort)
{
    switch (sort) {
    case CollectionSort::artist:
        if(a.authorNames.empty()!=b.authorNames.empty()) return a.authorNames.empty()?1:-1;
        return natural(a.authorNames,b.authorNames);
    case CollectionSort::number: return compare(a.work ? a.work->number : 0, b.work ? b.work->number : 0);
    case CollectionSort::title: return natural(a.title, b.title);
    case CollectionSort::version: return compare(a.version ? a.version->number : 0, b.version ? b.version->number : 0);
    case CollectionSort::name: return natural(a.name, b.name);
    case CollectionSort::date: return compare(a.file ? a.file->modified : 0, b.file ? b.file->modified : 0);
    case CollectionSort::size: return compare(a.file ? a.file->bytes : 0, b.file ? b.file->bytes : 0);
    case CollectionSort::extension: return natural(a.extension, b.extension);
    case CollectionSort::custom: return compare(a.order, b.order);
    }
    return 0;
}

struct Counts {
    std::size_t files = 0, works = 0, versions = 0, artists = 0, additional = 0;
};

Counts counts(const std::vector<const Item*>& items)
{
    std::set<LibraryId> works, versions, artists;
    std::set<const Item*> unique;
    std::map<LibraryId, std::set<LibraryId>> workVersions;
    for (const auto* item : items) {
        if (!unique.insert(item).second) continue;
        if (item->work) works.insert(item->work->id);
        if (item->version) {
            versions.insert(item->version->id);
            workVersions[item->version->artworkId].insert(item->version->id);
        }
        for (const auto* artist : item->artists) artists.insert(artist->id);
    }
    std::size_t additional = 0;
    for (const auto& [id, members] : workVersions) {
        (void) id;
        if (!members.empty()) additional += members.size() - 1;
    }
    return {unique.size(), works.size(), versions.size(), artists.size(), additional};
}

void appendFiles(CollectionViewResult& view, const std::vector<const Item*>& files, const std::vector<std::wstring>& ancestors)
{
    for (const auto* item : files) {
        CollectionEntry entry;
        entry.ancestorKeys = ancestors;
        entry.key = branch(ancestors.empty() ? L"" : ancestors.back(), L"file", pathKey(item->path));
        entry.label = item->name;
        entry.path = item->path;
        if (item->file) entry.mediaId = item->file->id;
        entry.files = 1;
        entry.artworks = item->work ? 1 : 0;
        entry.versions = item->version ? 1 : 0;
        view.rows.push_back(std::move(entry));
    }
}

bool appendGroup(CollectionViewResult& view, const std::wstring& key, std::wstring label,
                  const std::vector<const Item*>& files, const std::vector<std::wstring>& ancestors,
                  const CollectionViewOptions& options)
{
    const auto total = counts(files);
    CollectionEntry entry;
    entry.group = true;
    entry.key = key;
    entry.label = std::move(label);
    entry.ancestorKeys = ancestors;
    entry.files = total.files;
    entry.artworks = total.works;
    entry.versions = total.versions;
    view.rows.push_back(std::move(entry));
    return !options.collapsed.contains(key);
}

struct Bucket {
    std::wstring id, label;
    std::int64_t number = 0;
    std::vector<const Item*> files;
};

std::vector<Bucket> bucketsByWork(const std::vector<const Item*>& files)
{
    std::map<LibraryId, Bucket> groups;
    for (const auto* item : files) {
        const auto id = item->work ? item->work->id : LibraryId{};
        auto& group = groups[id];
        group.id = id;
        group.number = item->work ? item->work->number : 0;
        group.label = item->work ? (item->title.empty() ? L"Без названия" : item->title) : L"Без работы";
        group.files.push_back(item);
    }
    std::vector<Bucket> result;
    for (auto& [id, group] : groups) {
        (void) id;
        result.push_back(std::move(group));
    }
    std::ranges::sort(result, [](const Bucket& a, const Bucket& b) {
        return a.number != b.number ? a.number < b.number : natural(a.label, b.label) < 0;
    });
    return result;
}

void appendVersionGroups(CollectionViewResult& view, const Bucket& work, std::vector<std::wstring> ancestors,
                         const CollectionViewOptions& options)
{
    std::map<LibraryId, Bucket> versions;
    for (const auto* item : work.files) {
        const auto id = item->version ? item->version->id : LibraryId{};
        auto& version = versions[id];
        version.id = id;
        version.number = item->version ? item->version->number : 0;
        version.label = item->version ? item->version->title : std::wstring{};
        version.files.push_back(item);
    }
    std::vector<Bucket> ordered;
    for (auto& [id, version] : versions) {
        (void) id;
        ordered.push_back(std::move(version));
    }
    std::ranges::sort(ordered, [](const Bucket& a, const Bucket& b) {
        return a.number != b.number ? a.number < b.number : natural(a.label, b.label) < 0;
    });
    for (const auto& version : ordered) {
        if (version.id.empty()) {
            appendFiles(view, version.files, ancestors);
            continue;
        }
        const auto key = branch(ancestors.empty() ? L"" : ancestors.back(), L"version", version.id);
        auto label = L"v" + (version.number < 10 ? L"0" : std::wstring{}) + std::to_wstring(version.number);
        if (!version.label.empty()) label += L" · " + version.label;
        if (appendGroup(view, key, label, version.files, ancestors, options)) {
            auto childAncestors = ancestors;
            childAncestors.push_back(key);
            appendFiles(view, version.files, childAncestors);
        }
    }
}

void appendWorkGroups(CollectionViewResult& view, const std::vector<const Item*>& files,
                      const std::vector<std::wstring>& ancestors, const CollectionViewOptions& options)
{
    for (const auto& work : bucketsByWork(files)) {
        const auto key = branch(ancestors.empty() ? L"" : ancestors.back(), L"work", work.id);
        auto label = work.label;
        if (work.number > 0) {
            auto number = std::to_wstring(work.number);
            if (number.size() < 3) number.insert(0, 3 - number.size(), L'0');
            label = number + L" · " + label;
        }
        const auto total = counts(work.files);
        label += total.versions > 1 ? L" — " + std::to_wstring(total.versions) + L" версий"
                                   : L" — " + std::to_wstring(total.files) + L" файлов";
        if (appendGroup(view, key, label, work.files, ancestors, options)) {
            auto childAncestors = ancestors;
            childAncestors.push_back(key);
            appendVersionGroups(view, work, std::move(childAncestors), options);
        }
    }
}

void appendArtistGroups(CollectionViewResult& view, const std::vector<const Item*>& files,
                        const CollectionViewOptions& options)
{
    std::map<LibraryId, Bucket> artists;
    for (const auto* item : files) {
        if (item->artists.empty()) {
            auto& artist = artists[L""];
            artist.label = L"Без художника";
            artist.files.push_back(item);
        } else {
            for (const auto* author : item->artists) {
                auto& artist = artists[author->id];
                artist.id = author->id;
                artist.label = author->name;
                artist.number = author->order;
                artist.files.push_back(item);
            }
        }
    }
    std::vector<Bucket> ordered;
    for (auto& [id, artist] : artists) {
        (void) id;
        ordered.push_back(std::move(artist));
    }
    std::ranges::sort(ordered,[&options](const Bucket& a,const Bucket& b) {
        if(a.id.empty()!=b.id.empty()) return !a.id.empty();
        if(options.sort==CollectionSort::custom&&a.number!=b.number) return a.number<b.number;
        return natural(a.label,b.label)<0;
    });
    for (const auto& artist : ordered) {
        const auto key = branch(L"", L"artist", artist.id);
        const auto total = counts(artist.files);
        const bool showWorks=total.works&&!(options.filenameArtists&&artist.id.empty());
        const auto label=artist.label+L" — "+(showWorks?std::to_wstring(total.works)+L" работ · ":std::wstring{})+
            std::to_wstring(total.files)+L" файлов";
        const bool expanded=appendGroup(view,key,label,artist.files,{},options);
        view.rows.back().artistName=artist.label;
        if(expanded) {
            if(showWorks) appendWorkGroups(view,artist.files,{key},options);
            else appendFiles(view,artist.files,{key});
        }
    }
}

void appendSimpleGroups(CollectionViewResult& view, const std::vector<const Item*>& files,
                        const CollectionViewOptions& options)
{
    const bool formats = options.group == CollectionGroup::format;
    std::map<std::wstring, Bucket> groups;
    for (const auto* item : files) {
        const auto label = formats ? (item->extension.empty() ? L"Без формата" : folded(item->extension))
                                   : item->path.parent_path().wstring();
        auto& group = groups[folded(label)];
        group.label = label;
        group.files.push_back(item);
    }
    for (const auto& [id, group] : groups) {
        const auto key = branch(L"", formats ? L"format" : L"directory", id);
        const auto label = group.label + L" — " + std::to_wstring(group.files.size()) + L" файлов";
        if (appendGroup(view, key, label, group.files, {}, options)) appendFiles(view, group.files, {key});
    }
}

} // namespace

CollectionViewResult buildCollectionView(const CollectionSnapshot& collection,
                                   const std::vector<std::filesystem::path>& paths,
                                   const CollectionViewOptions& options)
{
    auto items = matchingItems(collection, paths, options);
    std::ranges::sort(items, [&options](const Item& a, const Item& b) {
        int order = compareItems(a, b, options.sort);
        if (order == 0) order = natural(a.name, b.name);
        if (order == 0) order = compare(a.ordinal, b.ordinal);
        return options.descending ? order > 0 : order < 0;
    });
    std::vector<const Item*> files;
    files.reserve(items.size());
    for (const auto& item : items) files.push_back(&item);
    const auto total = counts(files);
    CollectionViewResult result;
    result.fileCount = total.files;
    result.artworkCount = total.works;
    result.versionCount = total.versions;
    result.artistCount = total.artists;
    result.additionalVersions = total.additional;
    switch (options.group) {
    case CollectionGroup::none: appendFiles(result, files, {}); break;
    case CollectionGroup::artist: appendArtistGroups(result, files, options); break;
    case CollectionGroup::artwork: appendWorkGroups(result, files, {}, options); break;
    case CollectionGroup::format:
    case CollectionGroup::directory: appendSimpleGroups(result, files, options); break;
    }
    return result;
}

} // namespace viewery

