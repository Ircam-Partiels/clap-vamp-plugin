#include "cvp_hierarchy.h"
#include <algorithm>
#include <cctype>
#include <functional>

namespace Cvp
{
    static std::string trim(std::string text)
    {
        auto const isSpace = [](unsigned char c)
        {
            return std::isspace(c) != 0;
        };
        while(!text.empty() && isSpace(static_cast<unsigned char>(text.back())))
        {
            text.pop_back();
        }
        auto const first = std::find_if_not(text.cbegin(), text.cend(), [&](auto const c)
                                            {
                                                return isSpace(static_cast<unsigned char>(c));
                                            });
        return std::string(first, text.cend());
    }

    static std::vector<std::string> split(std::string const& label)
    {
        std::vector<std::string> levels;
        size_t start = 0;
        while(start <= label.size())
        {
            auto const end = std::min(label.find('/', start), label.size());
            auto level = trim(label.substr(start, end - start));
            if(!level.empty())
            {
                levels.push_back(std::move(level));
            }
            start = end + 1;
        }
        return levels;
    }

    std::string Hierarchy::normalize(std::string const& label)
    {
        auto text = label;
        std::replace_if(text.begin(), text.end(), [](auto const c)
                        {
                            return std::isspace(static_cast<unsigned char>(c)) != 0;
                        },
                        ' ');
        std::string result;
        for(auto const& level : split(text))
        {
            result += (result.empty() ? "" : "/") + level;
        }
        return result;
    }

    Hierarchy::Hierarchy(std::vector<std::string> const& labels)
    {
        // Each level of a label is searched among the children of the previous
        // level (or among the top-level nodes) and added if it does not exist.
        for(auto const& label : labels)
        {
            auto* siblings = &mRoots;
            std::string path;
            for(auto const& level : split(normalize(label)))
            {
                path += (path.empty() ? "" : "/") + level;
                auto it = std::find_if(siblings->begin(), siblings->end(), [&](auto const& node)
                                       {
                                           return node.name == level;
                                       });
                if(it == siblings->end())
                {
                    it = siblings->insert(siblings->end(), Node{level, path, 0, {}});
                }
                siblings = &it->children;
            }
        }

        // The nodes are numbered in depth-first order.
        std::function<void(std::vector<Node>&)> number = [&](std::vector<Node>& nodes)
        {
            for(auto& node : nodes)
            {
                node.index = mPaths.size();
                mPaths.push_back(node.path);
                number(node.children);
            }
        };
        number(mRoots);
    }

    std::vector<Hierarchy::Node> const& Hierarchy::getRoots() const
    {
        return mRoots;
    }

    std::vector<std::string> const& Hierarchy::getPaths() const
    {
        return mPaths;
    }
} // namespace Cvp
