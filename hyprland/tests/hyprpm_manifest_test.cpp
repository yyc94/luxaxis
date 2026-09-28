#include <toml++/toml.hpp>

#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "expected hyprpm.toml path\n";
        return 1;
    }

    try {
        const auto manifest = toml::parse_file(argv[1]);
        const auto repository = manifest["repository"].as_table();
        const auto plugin = manifest["luxaxis"].as_table();
        const auto repositoryName = manifest["repository"]["name"].value<std::string>();
        const auto output = manifest["luxaxis"]["output"].value<std::string>();
        const auto build = manifest["luxaxis"]["build"].as_array();

        if (manifest.size() != 2 || !repository || !plugin ||
            !repositoryName || *repositoryName != "luxaxis" ||
            !output || *output != "build-hyprpm/luxaxis.so" || !build || build->size() != 2) {
            std::cerr << "invalid hyprpm repository or plugin metadata\n";
            return 1;
        }

        const auto configure = build->get(0)->value<std::string>();
        const auto compile = build->get(1)->value<std::string>();
        if (!configure || !compile ||
            !configure->starts_with("cmake -S hyprland -B build-hyprpm ") ||
            !configure->contains("-DLUXAXIS_BUILD_TESTS=OFF") ||
            !compile->starts_with("cmake --build build-hyprpm --target luxaxis ")) {
            std::cerr << "hyprpm build steps do not produce the declared plugin target\n";
            return 1;
        }
    } catch (const toml::parse_error& error) {
        std::cerr << error << '\n';
        return 1;
    }

    return 0;
}
