#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace Cvp
{
    // The hierarchy organizes the labels (the text prompts or the classes of the
    // audio examples) as a tree. The levels of a label are separated by slashes,
    // for example "Animal/Bird/Owl" defines the nodes "Animal", "Animal/Bird" and
    // "Animal/Bird/Owl". A label without slash is a top-level node, hence a list
    // of labels without slash is a flat list of classes.
    //
    // The nodes are numbered in depth-first order (a parent precedes its
    // children, the siblings keep the order of their first appearance in the
    // labels): the index of a node is its position in the outputs (bins) and in
    // the vectors of scores.
    class Hierarchy
    {
    public:
        struct Node
        {
            std::string name;           // The last level of the path ("Bird")
            std::string path;           // The full path of the node ("Animal/Bird")
            size_t index{0};            // The index of the node in depth-first order
            std::vector<Node> children; // The child nodes
        };

        // Creates the hierarchy from the labels (the empty labels are ignored)
        explicit Hierarchy(std::vector<std::string> const& labels = {});

        // Returns the top-level nodes
        std::vector<Node> const& getRoots() const;

        // Returns the full paths of the nodes in depth-first order
        std::vector<std::string> const& getPaths() const;

        // Normalizes a label: the whitespaces are replaced by spaces, the levels
        // are trimmed and the empty levels are removed ("  Animal / Bird/ " is
        // normalized to "Animal/Bird").
        static std::string normalize(std::string const& label);

    private:
        std::vector<Node> mRoots;
        std::vector<std::string> mPaths;
    };
} // namespace Cvp
