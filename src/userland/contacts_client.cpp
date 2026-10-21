#include "aios/userland/contacts_client.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <charconv>
#if defined(_WIN32)
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
// std::stoi/std::stoul throw on a bad field, and bare metal is built with
// -fno-exceptions; from_chars never throws by design.
int parse_int(const std::string& s, int fallback = 0) {
  int v = fallback;
  std::from_chars(s.data(), s.data() + s.size(), v);
  return v;
}
unsigned long parse_hex(const std::string& s, unsigned long fallback) {
  unsigned long v = fallback;
  std::from_chars(s.data(), s.data() + s.size(), v, 16);
  return v;
}
}  // namespace

namespace aios::userland {

namespace {

void ensure_dir(const std::string& path) {
  mkdir(path.c_str(), 0755);
}

std::string trim_str(const std::string& s) {
  size_t start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

std::string escape_field(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (c == '\t' || c == '\n' || c == '\r') out += ' ';
    else out += c;
  }
  return out;
}

}  // namespace

ContactsStore::ContactsStore(const std::string& base_dir)
    : base_dir_(base_dir),
      db_path_(base_dir + "/contacts.idx"),
      cards_dir_(base_dir + "/cards") {
  ensure_dir(base_dir_);
  ensure_dir(cards_dir_);
  reload();
}

void ContactsStore::reload() {
  contacts_.clear();
  load_index();
}

void ContactsStore::save_all() {
  save_index();
  for (const auto& c : contacts_) {
    save_vcard_file(c);
  }
}

void ContactsStore::load_index() {
  std::ifstream f(db_path_);
  if (!f.is_open()) return;

  std::string line;
  while (std::getline(f, line)) {
    line = trim_str(line);
    if (line.empty() || line[0] == '#') continue;

    std::stringstream ss(line);
    std::string id, first, last, comp, title, email, email_w, phone, phone_w, addr, city, state, zip, country, notes, bday, tags_str, fav_str, color_str;
    
    std::getline(ss, id, '\t');
    std::getline(ss, first, '\t');
    std::getline(ss, last, '\t');
    std::getline(ss, comp, '\t');
    std::getline(ss, title, '\t');
    std::getline(ss, email, '\t');
    std::getline(ss, email_w, '\t');
    std::getline(ss, phone, '\t');
    std::getline(ss, phone_w, '\t');
    std::getline(ss, addr, '\t');
    std::getline(ss, city, '\t');
    std::getline(ss, state, '\t');
    std::getline(ss, zip, '\t');
    std::getline(ss, country, '\t');
    std::getline(ss, notes, '\t');
    std::getline(ss, bday, '\t');
    std::getline(ss, tags_str, '\t');
    std::getline(ss, fav_str, '\t');
    std::getline(ss, color_str, '\t');

    if (id.empty()) continue;

    ContactCard c;
    c.id = id;
    c.first_name = first;
    c.last_name = last;
    c.company = comp;
    c.job_title = title;
    c.email = email;
    c.email_work = email_w;
    c.phone = phone;
    c.phone_work = phone_w;
    c.address = addr;
    c.city = city;
    c.state = state;
    c.zip = zip;
    c.country = country;
    c.notes = notes;
    c.birthday = bday;
    c.is_favorite = (fav_str == "1" || fav_str == "true");
    c.avatar_color = color_str.empty() ? 0xFF0A84FF : parse_hex(color_str, 0xFF0A84FF);
    c.vcf_path = cards_dir_ + "/" + id + ".vcf";

    std::stringstream tag_ss(tags_str);
    std::string tag;
    while (std::getline(tag_ss, tag, ',')) {
      tag = trim_str(tag);
      if (!tag.empty()) c.tags.push_back(tag);
    }

    contacts_.push_back(c);
  }
}

void ContactsStore::save_index() {
  std::string tmp_path = db_path_ + ".tmp";
  std::ofstream f(tmp_path);
  if (!f.is_open()) return;

  f << "# AIOS Native Contacts Index (vCard Mirror)\n";
  f << "# id\tfirst\tlast\tcompany\ttitle\temail\temail_work\tphone\tphone_work\taddress\tcity\tstate\tzip\tcountry\tnotes\tbirthday\ttags\tfav\tcolor\n";

  for (const auto& c : contacts_) {
    std::string tags_str;
    for (size_t i = 0; i < c.tags.size(); ++i) {
      if (i > 0) tags_str += ",";
      tags_str += c.tags[i];
    }

    char col_buf[16];
    std::snprintf(col_buf, sizeof(col_buf), "0x%08X", c.avatar_color);

    f << escape_field(c.id) << "\t"
      << escape_field(c.first_name) << "\t"
      << escape_field(c.last_name) << "\t"
      << escape_field(c.company) << "\t"
      << escape_field(c.job_title) << "\t"
      << escape_field(c.email) << "\t"
      << escape_field(c.email_work) << "\t"
      << escape_field(c.phone) << "\t"
      << escape_field(c.phone_work) << "\t"
      << escape_field(c.address) << "\t"
      << escape_field(c.city) << "\t"
      << escape_field(c.state) << "\t"
      << escape_field(c.zip) << "\t"
      << escape_field(c.country) << "\t"
      << escape_field(c.notes) << "\t"
      << escape_field(c.birthday) << "\t"
      << escape_field(tags_str) << "\t"
      << (c.is_favorite ? "1" : "0") << "\t"
      << col_buf << "\n";
  }

  f.close();
  rename(tmp_path.c_str(), db_path_.c_str());
}

void ContactsStore::save_vcard_file(const ContactCard& c) {
  std::string path = cards_dir_ + "/" + c.id + ".vcf";
  std::ofstream f(path);
  if (!f.is_open()) return;

  f << "BEGIN:VCARD\r\n";
  f << "VERSION:3.0\r\n";
  f << "PRODID:-//AIOS//AetherContacts 1.0//EN\r\n";
  f << "UID:" << c.id << "\r\n";
  f << "N:" << c.last_name << ";" << c.first_name << ";;;\r\n";
  f << "FN:" << c.full_name() << "\r\n";
  if (!c.company.empty()) f << "ORG:" << c.company << "\r\n";
  if (!c.job_title.empty()) f << "TITLE:" << c.job_title << "\r\n";
  if (!c.email.empty()) f << "EMAIL;type=INTERNET;type=HOME:" << c.email << "\r\n";
  if (!c.email_work.empty()) f << "EMAIL;type=INTERNET;type=WORK:" << c.email_work << "\r\n";
  if (!c.phone.empty()) f << "TEL;type=CELL;type=VOICE:" << c.phone << "\r\n";
  if (!c.phone_work.empty()) f << "TEL;type=WORK;type=VOICE:" << c.phone_work << "\r\n";
  if (!c.address.empty() || !c.city.empty()) {
    f << "ADR;type=WORK:;;" << c.address << ";" << c.city << ";" << c.state << ";" << c.zip << ";" << c.country << "\r\n";
  }
  if (!c.notes.empty()) f << "NOTE:" << c.notes << "\r\n";
  if (!c.birthday.empty()) f << "BDAY:" << c.birthday << "\r\n";
  if (c.is_favorite) f << "X-AIOS-FAVORITE:TRUE\r\n";
  f << "END:VCARD\r\n";
}

void ContactsStore::delete_vcard_file(const std::string& id) {
  std::string path = cards_dir_ + "/" + id + ".vcf";
  unlink(path.c_str());
}


std::vector<ContactCard> ContactsStore::all_contacts() const {
  std::vector<ContactCard> res = contacts_;
  std::sort(res.begin(), res.end(), [](const ContactCard& a, const ContactCard& b) {
    if (a.last_name != b.last_name) return a.last_name < b.last_name;
    return a.first_name < b.first_name;
  });
  return res;
}

std::vector<ContactCard> ContactsStore::contacts_for_group(const std::string& group_id) const {
  if (group_id == "all" || group_id.empty()) return all_contacts();
  
  std::vector<ContactCard> res;
  if (group_id == "favorites") {
    for (const auto& c : contacts_) {
      if (c.is_favorite) res.push_back(c);
    }
  } else {
    for (const auto& c : contacts_) {
      for (const auto& t : c.tags) {
        if (t == group_id || (group_id == "team" && t == "QBC Team") ||
            (group_id == "engineering" && t == "Engineering") ||
            (group_id == "clients" && t == "Clients") ||
            (group_id == "vips" && t == "VIPs")) {
          res.push_back(c);
          break;
        }
      }
    }
  }

  std::sort(res.begin(), res.end(), [](const ContactCard& a, const ContactCard& b) {
    if (a.last_name != b.last_name) return a.last_name < b.last_name;
    return a.first_name < b.first_name;
  });
  return res;
}

const ContactCard* ContactsStore::get_contact(const std::string& id) const {
  for (const auto& c : contacts_) {
    if (c.id == id) return &c;
  }
  return nullptr;
}

bool ContactsStore::add_contact(ContactCard& card) {
  if (card.id.empty()) {
    int max_id = 100;
    for (const auto& c : contacts_) {
      if (c.id.rfind("ct-", 0) == 0) {
        int n = parse_int(c.id.substr(3), -1);
        if (n > max_id) max_id = n;
      }
    }
    card.id = "ct-" + std::to_string(max_id + 1);
  }
  card.vcf_path = cards_dir_ + "/" + card.id + ".vcf";
  if (card.avatar_color == 0) card.avatar_color = 0xFF0A84FF;
  
  contacts_.push_back(card);
  save_all();
  return true;
}

bool ContactsStore::update_contact(const ContactCard& card) {
  for (auto& c : contacts_) {
    if (c.id == card.id) {
      c = card;
      c.vcf_path = cards_dir_ + "/" + c.id + ".vcf";
      save_all();
      return true;
    }
  }
  return false;
}

bool ContactsStore::delete_contact(const std::string& id) {
  auto it = std::remove_if(contacts_.begin(), contacts_.end(), [&](const ContactCard& c) {
    return c.id == id;
  });
  if (it != contacts_.end()) {
    contacts_.erase(it, contacts_.end());
    delete_vcard_file(id);
    save_index();
    return true;
  }
  return false;
}

bool ContactsStore::toggle_favorite(const std::string& id) {
  for (auto& c : contacts_) {
    if (c.id == id) {
      c.is_favorite = !c.is_favorite;
      save_all();
      return true;
    }
  }
  return false;
}

std::vector<ContactCard> ContactsStore::search(const std::string& query) const {
  if (query.empty()) return all_contacts();
  std::string q = query;
  std::transform(q.begin(), q.end(), q.begin(), ::tolower);

  std::vector<ContactCard> res;
  for (const auto& c : contacts_) {
    std::string text = c.first_name + " " + c.last_name + " " + c.company + " " +
                       c.job_title + " " + c.email + " " + c.email_work + " " +
                       c.phone + " " + c.phone_work + " " + c.notes;
    std::transform(text.begin(), text.end(), text.begin(), ::tolower);
    if (text.find(q) != std::string::npos) {
      res.push_back(c);
    }
  }

  std::sort(res.begin(), res.end(), [](const ContactCard& a, const ContactCard& b) {
    if (a.last_name != b.last_name) return a.last_name < b.last_name;
    return a.first_name < b.first_name;
  });
  return res;
}

std::vector<ContactGroup> ContactsStore::get_groups() const {
  /* Groups are the tags the cards actually carry (vCard CATEGORIES). This
   * returned a fixed "QBC Team / Engineering / VIPs / Clients" list whether
   * or not a single card used them - four made-up groups on an empty book. */
  std::vector<ContactGroup> out;
  size_t fav_count = 0;
  std::vector<std::pair<std::string, size_t>> tags;
  for (const auto& c : contacts_) {
    if (c.is_favorite) fav_count++;
    for (const auto& t : c.tags) {
      if (t.empty()) continue;
      bool found = false;
      for (auto& kv : tags) if (kv.first == t) { ++kv.second; found = true; break; }
      if (!found) tags.push_back({t, 1});
    }
  }
  std::sort(tags.begin(), tags.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  out.push_back({"all", "All Contacts", "contacts", contacts_.size()});
  out.push_back({"favorites", "Favorites", "star", fav_count});
  for (const auto& kv : tags) out.push_back({kv.first, kv.first, "tag", kv.second});
  return out;
}

std::string ContactsStore::export_vcard(const std::string& id) const {
  const ContactCard* c = get_contact(id);
  if (!c) return "";
  std::ifstream f(c->vcf_path);
  if (!f.is_open()) return "";
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

std::string ContactsStore::export_all_vcards() const {
  std::stringstream ss;
  for (const auto& c : contacts_) {
    ss << export_vcard(c.id) << "\r\n";
  }
  return ss.str();
}

bool ContactsStore::import_vcard_data(const std::string& vcard_text) {
  std::stringstream ss(vcard_text);
  std::string line;
  ContactCard card;
  bool in_vcard = false;

  while (std::getline(ss, line)) {
    line = trim_str(line);
    if (line == "BEGIN:VCARD") {
      card = ContactCard();
      in_vcard = true;
    } else if (line == "END:VCARD") {
      if (in_vcard && (!card.first_name.empty() || !card.last_name.empty() || !card.email.empty())) {
        add_contact(card);
      }
      in_vcard = false;
    } else if (in_vcard) {
      if (line.rfind("FN:", 0) == 0) {
        std::string fn = line.substr(3);
        size_t sp = fn.find(' ');
        if (sp != std::string::npos) {
          card.first_name = fn.substr(0, sp);
          card.last_name = fn.substr(sp + 1);
        } else {
          card.first_name = fn;
        }
      } else if (line.rfind("ORG:", 0) == 0) {
        card.company = line.substr(4);
      } else if (line.rfind("TITLE:", 0) == 0) {
        card.job_title = line.substr(6);
      } else if (line.rfind("EMAIL", 0) == 0) {
        size_t col = line.find(':');
        if (col != std::string::npos) {
          std::string em = line.substr(col + 1);
          if (card.email.empty()) card.email = em;
          else card.email_work = em;
        }
      } else if (line.rfind("TEL", 0) == 0) {
        size_t col = line.find(':');
        if (col != std::string::npos) {
          std::string ph = line.substr(col + 1);
          if (card.phone.empty()) card.phone = ph;
          else card.phone_work = ph;
        }
      } else if (line.rfind("NOTE:", 0) == 0) {
        card.notes = line.substr(5);
      }
    }
  }
  return true;
}

std::string ContactsStore::status_line() const {
  return std::to_string(contacts_.size()) + " contacts • vCard RFC 6350 Store";
}

}  // namespace aios::userland
