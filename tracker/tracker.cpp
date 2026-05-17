/*
 * Tracker Server for Peer-to-Peer File Sharing System
 * Handles user management, group management, and file tracking
 */

#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <sstream>
#include <fstream>
#include <cstring>
#include <mutex>
#include <thread>
#include <algorithm>
#include <chrono>
#include <atomic>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>

using namespace std;

// Constants
const int BUFFER_SIZE = 65536;
const int PIECE_SIZE = 512 * 1024; // 512KB

// Global data structures with mutexes
mutex user_mutex;
mutex group_mutex;
mutex file_mutex;
mutex log_mutex;
mutex sync_mutex;

// User structure
struct User {
    string user_id;
    string password;
    bool is_logged_in;
    string ip;
    int port;
    set<string> groups; // Groups user belongs to
    set<string> shared_files; // Files being shared (group_id:filename)
};

// File info structure
struct FileInfo {
    string filename;
    string filepath;
    long long filesize;
    string sha1_hash; // Complete file SHA1
    vector<string> piece_hashes; // Piece-wise SHA1
    int num_pieces;
    set<string> seeders; // user_id of users sharing this file
};

// Group structure
struct Group {
    string group_id;
    string owner_id;
    set<string> members;
    set<string> pending_requests;
    map<string, FileInfo> files; // filename -> FileInfo
};

// Global maps
unordered_map<string, User> users;
unordered_map<string, Group> groups;
unordered_map<string, set<pair<string, int>>> file_peers; // group:file -> set of (ip, port)

// Tracker synchronization
vector<pair<string, int>> other_trackers;
int my_tracker_no;
atomic<bool> running(true);
int server_fd;

// Logging function
void log_message(const string& msg) {
    lock_guard<mutex> lock(log_mutex);
    ofstream log_file("trackerlog" + to_string(my_tracker_no) + ".txt", ios::app);
    auto now = chrono::system_clock::now();
    auto time = chrono::system_clock::to_time_t(now);
    log_file << ctime(&time) << msg << endl;
    log_file.close();
}

/*
 * Persistence helpers
 *
 * Persisted:  users (id + password + group memberships),
 *             groups (id + owner + members + pending requests + file metadata).
 * Not persisted: login state, IP/port, active seeders — all rebuilt dynamically
 *             when clients log back in (the client already re-registers shared
 *             files on every login).
 *
 * Format: one record per line, space-delimited tokens.
 *   USER       <user_id> <password>
 *   USER_GROUP <user_id> <group_id>
 *   GROUP      <group_id> <owner_id>
 *   GROUP_MEMBER  <group_id> <user_id>
 *   GROUP_PENDING <group_id> <user_id>
 *   GROUP_FILE    <group_id> <filename> <filesize> <sha1> <piece_hashes|NONE>
 *
 * Written to a temp file then renamed atomically to avoid corruption.
 */

// Write helper: keeps writing until all bytes are sent
static bool write_all(int fd, const string& s) {
    const char* p = s.c_str();
    size_t rem = s.size();
    while (rem > 0) {
        ssize_t n = write(fd, p, rem);
        if (n < 0) return false;
        p += n;
        rem -= n;
    }
    return true;
}

void save_state() {
    string fname = "tracker_state" + to_string(my_tracker_no) + ".dat";
    string tmp   = fname + ".tmp";

    // Build the complete state string under each mutex separately
    string out;

    {
        lock_guard<mutex> ulock(user_mutex);
        for (const auto& kv : users) {
            const User& u = kv.second;
            out += "USER " + u.user_id + " " + u.password + "\n";
            for (const auto& g : u.groups)
                out += "USER_GROUP " + u.user_id + " " + g + "\n";
        }
    }

    {
        lock_guard<mutex> glock(group_mutex);
        for (const auto& kv : groups) {
            const Group& g = kv.second;
            out += "GROUP " + g.group_id + " " + g.owner_id + "\n";
            for (const auto& m : g.members)
                out += "GROUP_MEMBER " + g.group_id + " " + m + "\n";
            for (const auto& p : g.pending_requests)
                out += "GROUP_PENDING " + g.group_id + " " + p + "\n";
            for (const auto& fkv : g.files) {
                const FileInfo& fi = fkv.second;
                string ph;
                for (const auto& h : fi.piece_hashes) ph += h;
                if (ph.empty()) ph = "NONE";
                out += "GROUP_FILE " + g.group_id + " " + fi.filename + " " +
                       to_string(fi.filesize) + " " + fi.sha1_hash + " " + ph + "\n";
            }
        }
    }

    // Write to temp file using system calls
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { log_message("save_state: open failed"); return; }

    if (!write_all(fd, out)) {
        close(fd);
        unlink(tmp.c_str());
        log_message("save_state: write failed");
        return;
    }
    close(fd);

    // Atomic replace
    if (rename(tmp.c_str(), fname.c_str()) < 0) {
        unlink(tmp.c_str());
        log_message("save_state: rename failed");
    }
}

void load_state() {
    string fname = "tracker_state" + to_string(my_tracker_no) + ".dat";

    int fd = open(fname.c_str(), O_RDONLY);
    if (fd < 0) return; // no state file — fresh start

    // Read entire file into a string
    string content;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        content.append(buf, n);
    close(fd);

    // Parse line by line
    stringstream ss(content);
    string line;
    while (getline(ss, line)) {
        stringstream ls(line);
        string tag; ls >> tag;
        if (tag.empty()) continue;

        if (tag == "USER") {
            string uid, pwd; ls >> uid >> pwd;
            if (uid.empty() || pwd.empty()) continue;
            User u;
            u.user_id = uid; u.password = pwd;
            u.is_logged_in = false; u.port = 0;
            users[uid] = u;
        }
        else if (tag == "USER_GROUP") {
            string uid, gid; ls >> uid >> gid;
            if (users.count(uid)) users[uid].groups.insert(gid);
        }
        else if (tag == "GROUP") {
            string gid, owner; ls >> gid >> owner;
            if (gid.empty() || owner.empty()) continue;
            Group g;
            g.group_id = gid; g.owner_id = owner;
            groups[gid] = g;
        }
        else if (tag == "GROUP_MEMBER") {
            string gid, uid; ls >> gid >> uid;
            if (groups.count(gid)) groups[gid].members.insert(uid);
        }
        else if (tag == "GROUP_PENDING") {
            string gid, uid; ls >> gid >> uid;
            if (groups.count(gid)) groups[gid].pending_requests.insert(uid);
        }
        else if (tag == "GROUP_FILE") {
            string gid, fname2, sha1, ph_str;
            long long fsz = 0;
            ls >> gid >> fname2 >> fsz >> sha1 >> ph_str;
            if (!groups.count(gid) || fname2.empty()) continue;
            FileInfo fi;
            fi.filename  = fname2;
            fi.filesize  = fsz;
            fi.sha1_hash = sha1;
            fi.num_pieces = (fsz + PIECE_SIZE - 1) / PIECE_SIZE;
            if (ph_str != "NONE") {
                for (size_t i = 0; i + 20 <= ph_str.size(); i += 20)
                    fi.piece_hashes.push_back(ph_str.substr(i, 20));
            }
            groups[gid].files[fname2] = fi;
        }
    }

    log_message("State restored from " + fname);
    cout << "State restored from " << fname << endl;
}

// Parse tracker info file
vector<pair<string, int>> parse_tracker_info(const string& filename) {
    vector<pair<string, int>> trackers;
    ifstream file(filename);
    string line;
    while (getline(file, line)) {
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon != string::npos) {
            string ip = line.substr(0, colon);
            int port = stoi(line.substr(colon + 1));
            trackers.push_back({ip, port});
        } else {
            // Space separated
            stringstream ss(line);
            string ip;
            int port;
            ss >> ip >> port;
            trackers.push_back({ip, port});
        }
    }
    return trackers;
}

// Tokenize command
vector<string> tokenize(const string& cmd) {
    vector<string> tokens;
    stringstream ss(cmd);
    string token;
    while (ss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

// Send sync message to other tracker
void sync_to_tracker(const string& msg) {
    lock_guard<mutex> lock(sync_mutex);
    for (auto& tracker : other_trackers) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(tracker.second);
        inet_pton(AF_INET, tracker.first.c_str(), &addr.sin_addr);
        
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            string sync_msg = "SYNC:" + msg;
            send(sock, sync_msg.c_str(), sync_msg.size(), 0);
        }
        close(sock);
    }
}

// Process sync message from other tracker
void process_sync(const string& msg) {
    vector<string> tokens = tokenize(msg);
    if (tokens.empty()) return;
    
    string cmd = tokens[0];
    
    if (cmd == "USER_CREATE" && tokens.size() >= 3) {
        lock_guard<mutex> lock(user_mutex);
        if (users.find(tokens[1]) == users.end()) {
            User u;
            u.user_id = tokens[1];
            u.password = tokens[2];
            u.is_logged_in = false;
            users[tokens[1]] = u;
        }
    }
    else if (cmd == "GROUP_CREATE" && tokens.size() >= 3) {
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(tokens[1]) == groups.end()) {
            Group g;
            g.group_id = tokens[1];
            g.owner_id = tokens[2];
            g.members.insert(tokens[2]);
            groups[tokens[1]] = g;
        }
    }
    else if (cmd == "GROUP_JOIN" && tokens.size() >= 3) {
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(tokens[1]) != groups.end()) {
            groups[tokens[1]].pending_requests.insert(tokens[2]);
        }
    }
    else if (cmd == "GROUP_ACCEPT" && tokens.size() >= 3) {
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(tokens[1]) != groups.end()) {
            groups[tokens[1]].pending_requests.erase(tokens[2]);
            groups[tokens[1]].members.insert(tokens[2]);
        }
        lock_guard<mutex> lock2(user_mutex);
        if (users.find(tokens[2]) != users.end()) {
            users[tokens[2]].groups.insert(tokens[1]);
        }
    }
    else if (cmd == "GROUP_LEAVE" && tokens.size() >= 3) {
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(tokens[1]) != groups.end()) {
            groups[tokens[1]].members.erase(tokens[2]);
            if (groups[tokens[1]].owner_id == tokens[2]) {
                if (!groups[tokens[1]].members.empty()) {
                    groups[tokens[1]].owner_id = *groups[tokens[1]].members.begin();
                }
            }
        }
        lock_guard<mutex> lock2(user_mutex);
        if (users.find(tokens[2]) != users.end()) {
            users[tokens[2]].groups.erase(tokens[1]);
        }
    }
    else if (cmd == "FILE_UPLOAD" && tokens.size() >= 6) {
        string group_id = tokens[1];
        string filename = tokens[2];
        string user_id = tokens[3];
        long long filesize = stoll(tokens[4]);
        string sha1 = tokens[5];
        string piece_hashes_str = (tokens.size() > 6) ? tokens[6] : "";
        
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(group_id) != groups.end()) {
            FileInfo fi;
            fi.filename = filename;
            fi.filesize = filesize;
            fi.sha1_hash = sha1;
            fi.num_pieces = (filesize + PIECE_SIZE - 1) / PIECE_SIZE;
            
            // Parse piece hashes — each piece contributes 20 hex chars (spec §File Integrity)
            if (!piece_hashes_str.empty()) {
                for (size_t i = 0; i < piece_hashes_str.length(); i += 20) {
                    fi.piece_hashes.push_back(piece_hashes_str.substr(i, 20));
                }
            }
            
            fi.seeders.insert(user_id);
            groups[group_id].files[filename] = fi;
        }
    }
    else if (cmd == "GROUP_REJECT" && tokens.size() >= 3) {
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(tokens[1]) != groups.end()) {
            groups[tokens[1]].pending_requests.erase(tokens[2]);
        }
    }
    else if (cmd == "FILE_SEEDER_ADD" && tokens.size() >= 4) {
        // Session-only: seeder list is rebuilt when clients log back in
        string group_id = tokens[1];
        string filename = tokens[2];
        string user_id = tokens[3];
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(group_id) != groups.end()) {
            if (groups[group_id].files.find(filename) != groups[group_id].files.end()) {
                groups[group_id].files[filename].seeders.insert(user_id);
            }
        }
        return; // not persisted
    }
    else if (cmd == "FILE_SEEDER_REMOVE" && tokens.size() >= 4) {
        // Session-only
        string group_id = tokens[1];
        string filename = tokens[2];
        string user_id = tokens[3];
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(group_id) != groups.end()) {
            if (groups[group_id].files.find(filename) != groups[group_id].files.end()) {
                groups[group_id].files[filename].seeders.erase(user_id);
            }
        }
        return; // not persisted
    }
    else if (cmd == "USER_LOGIN" && tokens.size() >= 4) {
        // Session-only
        lock_guard<mutex> lock(user_mutex);
        if (users.find(tokens[1]) != users.end()) {
            users[tokens[1]].is_logged_in = true;
            users[tokens[1]].ip = tokens[2];
            users[tokens[1]].port = stoi(tokens[3]);
        }
        return; // not persisted
    }
    else if (cmd == "USER_LOGOUT" && tokens.size() >= 2) {
        // Session-only
        lock_guard<mutex> lock(user_mutex);
        if (users.find(tokens[1]) != users.end()) {
            users[tokens[1]].is_logged_in = false;
        }
        return; // not persisted
    }

    // All other sync events change durable state — persist
    save_state();
}

// Handle create_user command
string handle_create_user(const vector<string>& tokens) {
    if (tokens.size() < 3) {
        return "ERROR:Invalid command. Usage: create_user <user_id> <password>";
    }
    
    string user_id = tokens[1];
    string password = tokens[2];
    
    lock_guard<mutex> lock(user_mutex);
    if (users.find(user_id) != users.end()) {
        return "ERROR:User already exists";
    }
    
    User u;
    u.user_id = user_id;
    u.password = password;
    u.is_logged_in = false;
    u.port = 0;
    users[user_id] = u;
    
    // Sync with other trackers
    sync_to_tracker("USER_CREATE " + user_id + " " + password);
    
    log_message("User created: " + user_id);
    return "SUCCESS:User created successfully";
}

// Handle login command
string handle_login(const vector<string>& tokens, const string& client_ip, int client_port) {
    if (tokens.size() < 3) {
        return "ERROR:Invalid command. Usage: login <user_id> <password>";
    }
    
    string user_id = tokens[1];
    string password = tokens[2];
    
    // Extract actual client port from the 4th token if provided
    int actual_port = client_port;
    if (tokens.size() >= 4) {
        actual_port = stoi(tokens[3]);
    }
    
    lock_guard<mutex> lock(user_mutex);
    if (users.find(user_id) == users.end()) {
        return "ERROR:User does not exist";
    }
    
    if (users[user_id].password != password) {
        return "ERROR:Invalid password";
    }
    
    if (users[user_id].is_logged_in) {
        return "ERROR:User already logged in";
    }
    
    users[user_id].is_logged_in = true;
    users[user_id].ip = client_ip;
    users[user_id].port = actual_port;
    
    // Sync with other trackers
    sync_to_tracker("USER_LOGIN " + user_id + " " + client_ip + " " + to_string(actual_port));
    
    log_message("User logged in: " + user_id + " at " + client_ip + ":" + to_string(actual_port));
    return "SUCCESS:Login successful";
}

// Handle logout command
string handle_logout(const vector<string>& tokens) {
    if (tokens.size() < 2) {
        return "ERROR:Invalid command. Usage: logout <user_id>";
    }
    
    string user_id = tokens[1];
    
    lock_guard<mutex> lock(user_mutex);
    if (users.find(user_id) == users.end()) {
        return "ERROR:User does not exist";
    }
    
    if (!users[user_id].is_logged_in) {
        return "ERROR:User not logged in";
    }
    
    users[user_id].is_logged_in = false;
    
    // Sync with other trackers
    sync_to_tracker("USER_LOGOUT " + user_id);
    
    log_message("User logged out: " + user_id);
    return "SUCCESS:Logout successful";
}

// Handle create_group command
string handle_create_group(const vector<string>& tokens) {
    if (tokens.size() < 3) {
        return "ERROR:Invalid command. Usage: create_group <group_id> <user_id>";
    }
    
    string group_id = tokens[1];
    string user_id = tokens[2];
    
    {
        lock_guard<mutex> lock(user_mutex);
        if (users.find(user_id) == users.end() || !users[user_id].is_logged_in) {
            return "ERROR:User not logged in";
        }
    }
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) != groups.end()) {
        return "ERROR:Group already exists";
    }
    
    Group g;
    g.group_id = group_id;
    g.owner_id = user_id;
    g.members.insert(user_id);
    groups[group_id] = g;
    
    {
        lock_guard<mutex> lock2(user_mutex);
        users[user_id].groups.insert(group_id);
    }
    
    // Sync with other trackers
    sync_to_tracker("GROUP_CREATE " + group_id + " " + user_id);
    
    log_message("Group created: " + group_id + " by " + user_id);
    return "SUCCESS:Group created successfully";
}

// Handle join_group command
string handle_join_group(const vector<string>& tokens) {
    if (tokens.size() < 3) {
        return "ERROR:Invalid command. Usage: join_group <group_id> <user_id>";
    }
    
    string group_id = tokens[1];
    string user_id = tokens[2];
    
    {
        lock_guard<mutex> lock(user_mutex);
        if (users.find(user_id) == users.end() || !users[user_id].is_logged_in) {
            return "ERROR:User not logged in";
        }
    }
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }
    
    if (groups[group_id].members.count(user_id)) {
        return "ERROR:Already a member of this group";
    }
    
    if (groups[group_id].pending_requests.count(user_id)) {
        return "ERROR:Request already pending";
    }
    
    groups[group_id].pending_requests.insert(user_id);
    
    // Sync with other trackers
    sync_to_tracker("GROUP_JOIN " + group_id + " " + user_id);
    
    log_message("Join request: " + user_id + " for group " + group_id);
    return "SUCCESS:Join request sent";
}

// Handle leave_group command
string handle_leave_group(const vector<string>& tokens) {
    if (tokens.size() < 3) {
        return "ERROR:Invalid command. Usage: leave_group <group_id> <user_id>";
    }
    
    string group_id = tokens[1];
    string user_id = tokens[2];
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }
    
    if (!groups[group_id].members.count(user_id)) {
        return "ERROR:Not a member of this group";
    }
    
    groups[group_id].members.erase(user_id);
    
    // If owner leaves, transfer ownership
    if (groups[group_id].owner_id == user_id) {
        if (!groups[group_id].members.empty()) {
            groups[group_id].owner_id = *groups[group_id].members.begin();
            log_message("Ownership transferred to: " + groups[group_id].owner_id);
        } else {
            // Group becomes empty, could delete it
            log_message("Group " + group_id + " is now empty");
        }
    }
    
    // Remove user from files they were seeding in this group
    for (auto& file_pair : groups[group_id].files) {
        file_pair.second.seeders.erase(user_id);
    }
    
    {
        lock_guard<mutex> lock2(user_mutex);
        users[user_id].groups.erase(group_id);
    }
    
    // Sync with other trackers
    sync_to_tracker("GROUP_LEAVE " + group_id + " " + user_id);
    
    log_message("User left group: " + user_id + " from " + group_id);
    return "SUCCESS:Left group successfully";
}

// Handle list_requests command
string handle_list_requests(const vector<string>& tokens) {
    if (tokens.size() < 3) {
        return "ERROR:Invalid command. Usage: list_requests <group_id> <user_id>";
    }
    
    string group_id = tokens[1];
    string user_id = tokens[2];
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }
    
    if (groups[group_id].owner_id != user_id) {
        return "ERROR:Only owner can view pending requests";
    }
    
    string result = "REQUESTS:";
    for (const auto& req : groups[group_id].pending_requests) {
        result += req + ",";
    }
    
    if (groups[group_id].pending_requests.empty()) {
        result += "No pending requests";
    } else {
        result.pop_back(); // Remove trailing comma
    }
    
    return result;
}

// Handle accept_request command
string handle_accept_request(const vector<string>& tokens) {
    if (tokens.size() < 4) {
        return "ERROR:Invalid command. Usage: accept_request <group_id> <user_id> <owner_id>";
    }
    
    string group_id = tokens[1];
    string request_user = tokens[2];
    string owner_id = tokens[3];
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }
    
    if (groups[group_id].owner_id != owner_id) {
        return "ERROR:Only owner can accept requests";
    }
    
    if (!groups[group_id].pending_requests.count(request_user)) {
        return "ERROR:No pending request from this user";
    }
    
    groups[group_id].pending_requests.erase(request_user);
    groups[group_id].members.insert(request_user);
    
    {
        lock_guard<mutex> lock2(user_mutex);
        users[request_user].groups.insert(group_id);
    }
    
    // Sync with other trackers
    sync_to_tracker("GROUP_ACCEPT " + group_id + " " + request_user);
    
    log_message("Request accepted: " + request_user + " to group " + group_id);
    return "SUCCESS:Request accepted";
}

// Handle reject_request command
string handle_reject_request(const vector<string>& tokens) {
    if (tokens.size() < 4) {
        return "ERROR:Invalid command. Usage: reject_request <group_id> <user_id> <owner_id>";
    }

    string group_id    = tokens[1];
    string request_user = tokens[2];
    string owner_id    = tokens[3];

    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }

    if (groups[group_id].owner_id != owner_id) {
        return "ERROR:Only owner can reject requests";
    }

    if (!groups[group_id].pending_requests.count(request_user)) {
        return "ERROR:No pending request from this user";
    }

    groups[group_id].pending_requests.erase(request_user);

    // Sync with other trackers
    sync_to_tracker("GROUP_REJECT " + group_id + " " + request_user);

    log_message("Request rejected: " + request_user + " from group " + group_id);
    return "SUCCESS:Request rejected";
}

// Handle list_groups command
string handle_list_groups(const vector<string>& /* tokens */) {
    lock_guard<mutex> lock(group_mutex);
    
    string result = "GROUPS:";
    for (const auto& g : groups) {
        result += g.first + ",";
    }
    
    if (groups.empty()) {
        result += "No groups available";
    } else {
        result.pop_back(); // Remove trailing comma
    }
    
    return result;
}

// Handle list_files command
string handle_list_files(const vector<string>& tokens) {
    if (tokens.size() < 3) {
        return "ERROR:Invalid command. Usage: list_files <group_id> <user_id>";
    }
    
    string group_id = tokens[1];
    string user_id = tokens[2];
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }
    
    if (!groups[group_id].members.count(user_id)) {
        return "ERROR:Not a member of this group";
    }
    
    string result = "FILES:";
    for (const auto& f : groups[group_id].files) {
        result += f.first + " (" + to_string(f.second.filesize) + " bytes),";
    }
    
    if (groups[group_id].files.empty()) {
        result += "No files shared in this group";
    } else {
        result.pop_back(); // Remove trailing comma
    }
    
    return result;
}

// Handle upload_file command
string handle_upload_file(const vector<string>& tokens) {
    if (tokens.size() < 7) {
        return "ERROR:Invalid command. Usage: upload_file <group_id> <filename> <filesize> <sha1> <piece_hashes> <user_id>";
    }
    
    string group_id = tokens[1];
    string filename = tokens[2];
    long long filesize = stoll(tokens[3]);
    string sha1 = tokens[4];
    string piece_hashes_str = tokens[5];
    string user_id = tokens[6];
    
    {
        lock_guard<mutex> lock(user_mutex);
        if (users.find(user_id) == users.end() || !users[user_id].is_logged_in) {
            return "ERROR:User not logged in";
        }
    }
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }
    
    if (!groups[group_id].members.count(user_id)) {
        return "ERROR:Not a member of this group";
    }
    
    FileInfo fi;
    fi.filename = filename;
    fi.filesize = filesize;
    fi.sha1_hash = sha1;
    fi.num_pieces = (filesize + PIECE_SIZE - 1) / PIECE_SIZE;
    
    // Parse piece hashes — each piece contributes 20 hex chars (spec §File Integrity)
    if (!piece_hashes_str.empty() && piece_hashes_str != "NONE") {
        for (size_t i = 0; i < piece_hashes_str.length(); i += 20) {
            if (i + 20 <= piece_hashes_str.length()) {
                fi.piece_hashes.push_back(piece_hashes_str.substr(i, 20));
            }
        }
    }
    
    fi.seeders.insert(user_id);
    
    // Check if file already exists
    if (groups[group_id].files.find(filename) != groups[group_id].files.end()) {
        // Add user as a seeder
        groups[group_id].files[filename].seeders.insert(user_id);
    } else {
        groups[group_id].files[filename] = fi;
    }
    
    // Sync with other trackers
    sync_to_tracker("FILE_UPLOAD " + group_id + " " + filename + " " + user_id + " " + 
                    to_string(filesize) + " " + sha1 + " " + piece_hashes_str);
    
    log_message("File uploaded: " + filename + " to group " + group_id + " by " + user_id);
    return "SUCCESS:File uploaded successfully";
}

// Handle get_peers command (for downloading)
string handle_get_peers(const vector<string>& tokens) {
    if (tokens.size() < 4) {
        return "ERROR:Invalid command. Usage: get_peers <group_id> <filename> <user_id>";
    }
    
    string group_id = tokens[1];
    string filename = tokens[2];
    string user_id = tokens[3];
    
    {
        lock_guard<mutex> lock(user_mutex);
        if (users.find(user_id) == users.end() || !users[user_id].is_logged_in) {
            return "ERROR:User not logged in";
        }
    }
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }
    
    if (!groups[group_id].members.count(user_id)) {
        return "ERROR:Not a member of this group";
    }
    
    if (groups[group_id].files.find(filename) == groups[group_id].files.end()) {
        return "ERROR:File not found in group";
    }
    
    FileInfo& fi = groups[group_id].files[filename];
    
    string result = "PEERS:";
    result += to_string(fi.filesize) + "|";
    result += fi.sha1_hash + "|";
    
    // Concatenate piece hashes
    string all_piece_hashes = "";
    for (const auto& ph : fi.piece_hashes) {
        all_piece_hashes += ph;
    }
    result += all_piece_hashes + "|";
    
    // Get active seeders
    lock_guard<mutex> ulock(user_mutex);
    for (const auto& seeder : fi.seeders) {
        if (users.find(seeder) != users.end() && users[seeder].is_logged_in) {
            result += users[seeder].ip + ":" + to_string(users[seeder].port) + ",";
        }
    }
    
    if (result.back() == '|') {
        result += "NO_ACTIVE_PEERS";
    } else {
        result.pop_back(); // Remove trailing comma
    }
    
    return result;
}

// Handle add_seeder command (when download completes)
string handle_add_seeder(const vector<string>& tokens) {
    if (tokens.size() < 4) {
        return "ERROR:Invalid command. Usage: add_seeder <group_id> <filename> <user_id>";
    }
    
    string group_id = tokens[1];
    string filename = tokens[2];
    string user_id = tokens[3];
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }
    
    if (groups[group_id].files.find(filename) == groups[group_id].files.end()) {
        return "ERROR:File not found";
    }
    
    groups[group_id].files[filename].seeders.insert(user_id);
    
    // Sync with other trackers
    sync_to_tracker("FILE_SEEDER_ADD " + group_id + " " + filename + " " + user_id);
    
    log_message("Seeder added: " + user_id + " for file " + filename);
    return "SUCCESS:Added as seeder";
}

// Handle stop_share command
string handle_stop_share(const vector<string>& tokens) {
    if (tokens.size() < 4) {
        return "ERROR:Invalid command. Usage: stop_share <group_id> <filename> <user_id>";
    }
    
    string group_id = tokens[1];
    string filename = tokens[2];
    string user_id = tokens[3];
    
    lock_guard<mutex> lock(group_mutex);
    if (groups.find(group_id) == groups.end()) {
        return "ERROR:Group does not exist";
    }
    
    if (groups[group_id].files.find(filename) == groups[group_id].files.end()) {
        return "ERROR:File not found";
    }
    
    groups[group_id].files[filename].seeders.erase(user_id);
    
    // Sync with other trackers
    sync_to_tracker("FILE_SEEDER_REMOVE " + group_id + " " + filename + " " + user_id);
    
    log_message("Stopped sharing: " + filename + " by " + user_id);
    return "SUCCESS:Stopped sharing file";
}

// Commands that modify durable state and must trigger a state save
static bool is_persistent_command(const string& command) {
    return command == "create_user"   ||
           command == "create_group"  ||
           command == "join_group"    ||
           command == "leave_group"   ||
           command == "accept_request"||
           command == "reject_request"||
           command == "upload_file";
}

// Process client command
string process_command(const string& cmd, const string& client_ip, int client_port) {
    vector<string> tokens = tokenize(cmd);
    if (tokens.empty()) {
        return "ERROR:Empty command";
    }

    string command = tokens[0];
    string response;

    if (command == "create_user") {
        response = handle_create_user(tokens);
    }
    else if (command == "login") {
        response = handle_login(tokens, client_ip, client_port);
    }
    else if (command == "logout") {
        response = handle_logout(tokens);
    }
    else if (command == "create_group") {
        response = handle_create_group(tokens);
    }
    else if (command == "join_group") {
        response = handle_join_group(tokens);
    }
    else if (command == "leave_group") {
        response = handle_leave_group(tokens);
    }
    else if (command == "list_requests") {
        response = handle_list_requests(tokens);
    }
    else if (command == "accept_request") {
        response = handle_accept_request(tokens);
    }
    else if (command == "reject_request") {
        response = handle_reject_request(tokens);
    }
    else if (command == "list_groups") {
        response = handle_list_groups(tokens);
    }
    else if (command == "list_files") {
        response = handle_list_files(tokens);
    }
    else if (command == "upload_file") {
        response = handle_upload_file(tokens);
    }
    else if (command == "get_peers") {
        response = handle_get_peers(tokens);
    }
    else if (command == "add_seeder") {
        response = handle_add_seeder(tokens);
    }
    else if (command == "stop_share") {
        response = handle_stop_share(tokens);
    }
    else {
        return "ERROR:Unknown command";
    }

    // Persist state after any command that changes durable data
    if (response.substr(0, 7) == "SUCCESS" && is_persistent_command(command)) {
        save_state();
    }

    return response;
}

// Handle client connection
void handle_client(int client_fd, string client_ip, int client_port) {
    char buffer[BUFFER_SIZE];
    
    while (running) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes_read <= 0) {
            break;
        }
        
        string cmd(buffer);
        
        // Check if it's a sync message from another tracker
        if (cmd.substr(0, 5) == "SYNC:") {
            process_sync(cmd.substr(5));
            close(client_fd);
            return;
        }
        
        log_message("Received from " + client_ip + ":" + to_string(client_port) + " - " + cmd);
        
        string response = process_command(cmd, client_ip, client_port);
        send(client_fd, response.c_str(), response.size(), 0);
        
        log_message("Response: " + response);
    }
    
    close(client_fd);
}

// Signal handler
void signal_handler(int /* sig */) {
    running = false;
    close(server_fd);
}

// Quit handler thread
void quit_handler() {
    string input;
    while (running) {
        getline(cin, input);
        if (input == "quit") {
            cout << "Tracker shutting down..." << endl;
            running = false;
            close(server_fd);
            break;
        }
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Usage: ./tracker tracker_info.txt tracker_no" << endl;
        return 1;
    }
    
    string tracker_info_file = argv[1];
    my_tracker_no = stoi(argv[2]);
    
    // Parse tracker info
    vector<pair<string, int>> all_trackers = parse_tracker_info(tracker_info_file);
    
    if (my_tracker_no < 1 || my_tracker_no > (int)all_trackers.size()) {
        cerr << "Invalid tracker number" << endl;
        return 1;
    }
    
    // Get my tracker info
    string my_ip = all_trackers[my_tracker_no - 1].first;
    int my_port = all_trackers[my_tracker_no - 1].second;
    
    // Store other trackers for sync
    for (int i = 0; i < (int)all_trackers.size(); i++) {
        if (i != my_tracker_no - 1) {
            other_trackers.push_back(all_trackers[i]);
        }
    }
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        cerr << "Socket creation failed" << endl;
        return 1;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(my_port);
    
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        cerr << "Bind failed on port " << my_port << endl;
        return 1;
    }
    
    // Listen
    if (listen(server_fd, 100) < 0) {
        cerr << "Listen failed" << endl;
        return 1;
    }
    
    // Restore persisted state before accepting any connections
    load_state();

    cout << "Tracker " << my_tracker_no << " started on " << my_ip << ":" << my_port << endl;
    log_message("Tracker started on " + my_ip + ":" + to_string(my_port));
    
    // Start quit handler thread
    thread quit_thread(quit_handler);
    quit_thread.detach();
    
    // Accept connections
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (running) {
                log_message("Accept failed");
            }
            continue;
        }
        
        string client_ip = inet_ntoa(client_addr.sin_addr);
        int client_port = ntohs(client_addr.sin_port);
        
        thread client_thread(handle_client, client_fd, client_ip, client_port);
        client_thread.detach();
    }
    
    cout << "Tracker stopped" << endl;
    return 0;
}
