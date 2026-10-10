#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace kon {
// One executable; disc content changes with the selected language. The host
// must return to a safe reload point before replacing already loaded resources.
class ContentMount {
public:
    explicit ContentMount(std::filesystem::path project_root, std::string language = "en")
        : root_(std::filesystem::weakly_canonical(std::move(project_root))) {
        select(std::move(language));
    }
    const std::string& language() const noexcept { return language_; }
    const std::filesystem::path& root() const noexcept { return root_; }
    static bool valid_language_id(std::string_view id){
        if(id.empty() || id.size()>63 || id.front()=='-' || id.back()=='-')return false;
        return std::all_of(id.begin(),id.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-';});
    }
    std::vector<std::string> available_languages()const{
        std::vector<std::string> result;
        for(const auto id:{"ja","en"})if(std::filesystem::is_regular_file(language_base(id)/"PSP_GAME/USRDIR/std.cpk"))result.emplace_back(id);
        const auto directory=root_/"assets/locales";
        if(std::filesystem::is_directory(directory))for(const auto& entry:std::filesystem::directory_iterator(directory)){
            const auto id=entry.path().filename().string();
            if(id!="ja" && id!="en" && valid_language_id(id) && entry.is_directory() && std::filesystem::is_regular_file(entry.path()/"PSP_GAME/USRDIR/std.cpk")){
                (void)checked(entry.path()/"PSP_GAME/USRDIR/std.cpk");result.push_back(id);
            }
        }
        std::sort(result.begin(),result.end());return result;
    }
    static std::string disc_relative(std::string input) {
        std::replace(input.begin(), input.end(), '\\', '/');
        if (const auto colon = input.find(':'); colon != std::string::npos) {
            const auto device = input.substr(0, colon);
            if (device != "disc0" && device != "umd0")
                throw std::invalid_argument("Unsupported disc device: " + device);
            input.erase(0, colon + 1);
        }
        while (!input.empty() && input.front() == '/') input.erase(input.begin());
        if (input.empty()) throw std::invalid_argument("Empty disc file path");
        std::filesystem::path result;
        for (const auto& part : std::filesystem::path(input)) {
            const auto component = part.string();
            if (component == ".." || component.find(':') != std::string::npos)
                throw std::invalid_argument("Unsafe disc path");
            if (component != ".") result /= part;
        }
        return result.generic_string();
    }
    void select(std::string language) {
        if (!valid_language_id(language))throw std::invalid_argument("Invalid language identifier: " + language);
        const auto base = language_base(language);
        if (!std::filesystem::is_regular_file(base / "PSP_GAME/USRDIR/std.cpk"))
            throw std::runtime_error("Language resources unavailable: " + language);
        (void)checked(base / "PSP_GAME/USRDIR/std.cpk");
        selected_ = base;
        language_ = std::move(language);
    }
    // Folders whose files replace disc files (mods/<name>/disc), lowest
    // priority first; the last one holding a file wins.
    void set_disc_overrides(std::vector<std::filesystem::path> folders){overrides_=std::move(folders);}
    std::filesystem::path resolve(const std::string& guest_path) const {
        const auto relative = disc_relative(guest_path);
        for(auto it=overrides_.rbegin();it!=overrides_.rend();++it)
            if(const auto candidate=*it/relative;std::filesystem::is_regular_file(candidate))return checked(candidate);
        // Both source sound banks have identical SHA256; prefer one common copy.
        if (relative == "PSP_GAME/USRDIR/snd.cpk")
            return checked(root_ / "analysis/extracted/jp" / relative);
        const auto localized = selected_ / relative;
        if (std::filesystem::is_regular_file(localized)) return checked(localized);
        return checked(root_ / "analysis/extracted/jp" / relative);
    }
    static std::string read_setting(const std::filesystem::path& file) {
        if (!std::filesystem::exists(file)) return "en";
        std::ifstream input(file);
        if (!input) throw std::runtime_error("Cannot open language settings");
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.starts_with("language=")) return line.substr(9);
        }
        throw std::runtime_error("Missing language setting");
    }
    void save_setting(const std::filesystem::path& file) const {
        const auto parent = file.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        std::ofstream output(file, std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write language settings");
        output << "language=" << language_ << '\n';
        output.flush();
        if (!output) throw std::runtime_error("Cannot persist language settings");
    }
private:
    std::filesystem::path language_base(std::string_view language)const{
        if(language=="ja" || language=="en")return root_/"analysis/extracted"/(language=="ja"?"jp":"en");
        return root_/"assets/locales"/std::string(language);
    }
    std::filesystem::path checked(const std::filesystem::path& candidate) const {
        const auto canonical = std::filesystem::weakly_canonical(candidate);
        const auto relative = canonical.lexically_relative(root_);
        if (relative.empty() || *relative.begin() == ".." || !std::filesystem::is_regular_file(canonical))
            throw std::runtime_error("Missing or invalid disc file: " + candidate.string());
        return canonical;
    }
    std::filesystem::path root_, selected_;
    std::vector<std::filesystem::path> overrides_;
    std::string language_;
};
}
