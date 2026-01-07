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
#include <unordered_set>
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
#include <pthread.h>

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
            
            // Parse piece hashes
            if (!piece_hashes_str.empty()) {
                for (size_t i = 0; i < piece_hashes_str.length(); i += 40) {
                    fi.piece_hashes.push_back(piece_hashes_str.substr(i, 40));
                }
            }
            
            fi.seeders.insert(user_id);
            groups[group_id].files[filename] = fi;
        }
    }
    else if (cmd == "FILE_SEEDER_ADD" && tokens.size() >= 4) {
        string group_id = tokens[1];
        string filename = tokens[2];
        string user_id = tokens[3];
        
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(group_id) != groups.end()) {
            if (groups[group_id].files.find(filename) != groups[group_id].files.end()) {
                groups[group_id].files[filename].seeders.insert(user_id);
            }
        }
    }
    else if (cmd == "FILE_SEEDER_REMOVE" && tokens.size() >= 4) {
        string group_id = tokens[1];
        string filename = tokens[2];
        string user_id = tokens[3];
        
        lock_guard<mutex> lock(group_mutex);
        if (groups.find(group_id) != groups.end()) {
            if (groups[group_id].files.find(filename) != groups[group_id].files.end()) {
                groups[group_id].files[filename].seeders.erase(user_id);
            }
        }
    }
    else if (cmd == "USER_LOGIN" && tokens.size() >= 4) {
        lock_guard<mutex> lock(user_mutex);
        if (users.find(tokens[1]) != users.end()) {
            users[tokens[1]].is_logged_in = true;
            users[tokens[1]].ip = tokens[2];
            users[tokens[1]].port = stoi(tokens[3]);
        }
    }
    else if (cmd == "USER_LOGOUT" && tokens.size() >= 2) {
        lock_guard<mutex> lock(user_mutex);
        if (users.find(tokens[1]) != users.end()) {
            users[tokens[1]].is_logged_in = false;
        }
    }
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
    
    // Parse piece hashes
    if (!piece_hashes_str.empty() && piece_hashes_str != "NONE") {
        for (size_t i = 0; i < piece_hashes_str.length(); i += 40) {
            if (i + 40 <= piece_hashes_str.length()) {
                fi.piece_hashes.push_back(piece_hashes_str.substr(i, 40));
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

// Process client command
string process_command(const string& cmd, const string& client_ip, int client_port) {
    vector<string> tokens = tokenize(cmd);
    if (tokens.empty()) {
        return "ERROR:Empty command";
    }
    
    string command = tokens[0];
    
    if (command == "create_user") {
        return handle_create_user(tokens);
    }
    else if (command == "login") {
        return handle_login(tokens, client_ip, client_port);
    }
    else if (command == "logout") {
        return handle_logout(tokens);
    }
    else if (command == "create_group") {
        return handle_create_group(tokens);
    }
    else if (command == "join_group") {
        return handle_join_group(tokens);
    }
    else if (command == "leave_group") {
        return handle_leave_group(tokens);
    }
    else if (command == "list_requests") {
        return handle_list_requests(tokens);
    }
    else if (command == "accept_request") {
        return handle_accept_request(tokens);
    }
    else if (command == "list_groups") {
        return handle_list_groups(tokens);
    }
    else if (command == "list_files") {
        return handle_list_files(tokens);
    }
    else if (command == "upload_file") {
        return handle_upload_file(tokens);
    }
    else if (command == "get_peers") {
        return handle_get_peers(tokens);
    }
    else if (command == "add_seeder") {
        return handle_add_seeder(tokens);
    }
    else if (command == "stop_share") {
        return handle_stop_share(tokens);
    }
    else {
        return "ERROR:Unknown command";
    }
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
