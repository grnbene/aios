#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "aios/core/result.hpp"

namespace aios::userland {

struct ContactCard {
  std::string id;
  std::string first_name;
  std::string last_name;
  std::string company;
  std::string department;
  std::string job_title;
  std::string email;
  std::string email_work;
  std::string phone;
  std::string phone_work;
  std::string address;
  std::string city;
  std::string state;
  std::string zip;
  std::string country;
  std::string notes;
  std::string birthday;
  std::string vcf_path;
  std::vector<std::string> tags; // "Favorites", "QBC Team", "Engineering", "VIPs", "Clients"
  bool is_favorite = false;
  uint32_t avatar_color = 0xFF0A84FF;

  std::string full_name() const {
    if (!first_name.empty() && !last_name.empty()) {
      return first_name + " " + last_name;
    }
    if (!first_name.empty()) return first_name;
    if (!last_name.empty()) return last_name;
    if (!company.empty()) return company;
    if (!email.empty()) return email;
    return "Unknown Contact";
  }

  std::string initials() const {
    std::string res;
    if (!first_name.empty()) {
      char c = first_name[0];
      if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
      res += c;
    }
    if (!last_name.empty()) {
      char c = last_name[0];
      if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
      res += c;
    }
    if (res.empty() && !company.empty()) {
      char c = company[0];
      if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
      res += c;
    }
    return res.empty() ? "??" : res;
  }
};

struct ContactGroup {
  std::string id;
  std::string name;
  std::string icon;
  size_t count = 0;
};

class ContactsStore {
 public:
  explicit ContactsStore(const std::string& base_dir);
  ~ContactsStore() = default;

  void reload();
  void save_all();

  std::vector<ContactCard> all_contacts() const;
  std::vector<ContactCard> contacts_for_group(const std::string& group_id) const;
  const ContactCard* get_contact(const std::string& id) const;
  
  // Assigns card.id (and its vcf path) in place, so the caller can name what it made.
  bool add_contact(ContactCard& card);
  bool update_contact(const ContactCard& card);
  bool delete_contact(const std::string& id);
  bool toggle_favorite(const std::string& id);

  std::vector<ContactCard> search(const std::string& query) const;
  std::vector<ContactGroup> get_groups() const;

  bool import_vcard_data(const std::string& vcard_text);
  std::string export_vcard(const std::string& id) const;
  std::string export_all_vcards() const;

  size_t total_contacts() const { return contacts_.size(); }
  std::string status_line() const;

 private:
  std::string base_dir_;
  std::string db_path_;
  std::string cards_dir_;
  std::vector<ContactCard> contacts_;

  void load_index();
  void save_index();
  void save_vcard_file(const ContactCard& card);
  void delete_vcard_file(const std::string& id);
};

}  // namespace aios::userland
