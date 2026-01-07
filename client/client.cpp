/*
 * Client for Peer-to-Peer File Sharing System
 * Handles file sharing, downloading, and peer-to-peer communication
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
#include <atomic>
#include <condition_variable>
#include <queue>
#include <random>
#include <chrono>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <pthread.h>
#include <openssl/evp.h>
#include <dirent.h>

using namespace std;

// Constants
const int BUFFER_SIZE = 65536;
const int PIECE_SIZE = 512 * 1024; // 512KB
const int MAX_PEERS_PER_PIECE = 4;

// Global variables
string my_ip;
int my_port;
string current_user;
bool is_logged_in = false;
atomic<bool> running(true);
int peer_server_fd = -1;

// Mutexes
mutex download_mutex;
mutex share_mutex;
mutex cout_mutex;

// Tracker info
vector<pair<string, int>> trackers;

// File piece info
struct PieceInfo {
    int piece_index;
    bool is_complete;
    string hash;
    vector<char> data;
};

// Download info
struct DownloadInfo {
    string group_id;
    string filename;
    string dest_path;
    long long filesize;
    string file_hash;
    vector<string> piece_hashes;
    int total_pieces;
    vector<bool> pieces_completed;
    atomic<bool> is_complete;
    atomic<bool> is_downloading;
    
    DownloadInfo() : is_complete(false), is_downloading(false) {}
};

// Shared file info
struct SharedFile {
    string group_id;
    string filename;
    string filepath;
    long long filesize;
    string file_hash;
    vector<string> piece_hashes;
    int num_pieces;
    vector<bool> pieces_available;
};

// Global maps
map<string, DownloadInfo*> active_downloads; // group:filename -> DownloadInfo
map<string, SharedFile> shared_files; // group:filename -> SharedFile

// Compute SHA1 hash using EVP API (OpenSSL 3.0 compatible)
string compute_sha1(const char* data, size_t len) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    
    char hex_hash[41];
    for (unsigned int i = 0; i < 20; i++) {
        sprintf(hex_hash + i * 2, "%02x", hash[i]);
    }
    hex_hash[40] = '\0';
    return string(hex_hash);
}

// Compute SHA1 hash of a file using EVP API
string compute_file_sha1(const string& filepath) {
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) return "";
    
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha1(), NULL);
    
    char buffer[PIECE_SIZE];
    ssize_t bytes_read;
    while ((bytes_read = read(fd, buffer, PIECE_SIZE)) > 0) {
        EVP_DigestUpdate(ctx, buffer, bytes_read);
    }
    
    close(fd);
    
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);
    
    char hex_hash[41];
    for (unsigned int i = 0; i < 20; i++) {
        sprintf(hex_hash + i * 2, "%02x", hash[i]);
    }
    hex_hash[40] = '\0';
    return string(hex_hash);
}

// Compute piece-wise SHA1 hashes
vector<string> compute_piece_hashes(const string& filepath, long long /* filesize */) {
    vector<string> hashes;
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) return hashes;
    
    char buffer[PIECE_SIZE];
    ssize_t bytes_read;
    
    while ((bytes_read = read(fd, buffer, PIECE_SIZE)) > 0) {
        string hash = compute_sha1(buffer, bytes_read);
        hashes.push_back(hash.substr(0, 40)); // Use first 20 bytes (40 hex chars)
    }
    
    close(fd);
    return hashes;
}

// Get file size
long long get_file_size(const string& filepath) {
    struct stat st;
    if (stat(filepath.c_str(), &st) == 0) {
        return st.st_size;
    }
    return -1;
}

// Parse tracker info file
vector<pair<string, int>> parse_tracker_info(const string& filename) {
    vector<pair<string, int>> result;
    ifstream file(filename);
    string line;
    while (getline(file, line)) {
        if (line.empty()) continue;
        size_t colon = line.find(':');
        if (colon != string::npos) {
            string ip = line.substr(0, colon);
            int port = stoi(line.substr(colon + 1));
            result.push_back({ip, port});
        } else {
            stringstream ss(line);
            string ip;
            int port;
            ss >> ip >> port;
            result.push_back({ip, port});
        }
    }
    return result;
}

// Connect to tracker
int connect_to_tracker() {
    for (const auto& tracker : trackers) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;
        
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(tracker.second);
        inet_pton(AF_INET, tracker.first.c_str(), &addr.sin_addr);
        
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
        
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            return sock;
        }
        close(sock);
    }
    return -1;
}

// Send command to tracker
string send_to_tracker(const string& cmd) {
    int sock = connect_to_tracker();
    if (sock < 0) {
        return "ERROR:Could not connect to tracker";
    }
    
    send(sock, cmd.c_str(), cmd.size(), 0);
    
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    int bytes_read = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    
    close(sock);
    
    if (bytes_read <= 0) {
        return "ERROR:No response from tracker";
    }
    
    return string(buffer);
}

// Tokenize string
vector<string> tokenize(const string& str, char delim = ' ') {
    vector<string> tokens;
    stringstream ss(str);
    string token;
    while (getline(ss, token, delim)) {
        if (!token.empty()) {
            tokens.push_back(token);
        }
    }
    return tokens;
}

// Get file piece data
bool get_piece_data(const string& filepath, int piece_index, vector<char>& data) {
    int fd = open(filepath.c_str(), O_RDONLY);
    if (fd < 0) return false;
    
    off_t offset = (off_t)piece_index * PIECE_SIZE;
    if (lseek(fd, offset, SEEK_SET) < 0) {
        close(fd);
        return false;
    }
    
    data.resize(PIECE_SIZE);
    ssize_t bytes_read = read(fd, data.data(), PIECE_SIZE);
    close(fd);
    
    if (bytes_read <= 0) return false;
    data.resize(bytes_read);
    return true;
}

// Write piece data to file
bool write_piece_data(const string& filepath, int piece_index, const vector<char>& data) {
    int fd = open(filepath.c_str(), O_WRONLY | O_CREAT, 0644);
    if (fd < 0) return false;
    
    off_t offset = (off_t)piece_index * PIECE_SIZE;
    if (lseek(fd, offset, SEEK_SET) < 0) {
        close(fd);
        return false;
    }
    
    ssize_t bytes_written = write(fd, data.data(), data.size());
    close(fd);
    
    return bytes_written == (ssize_t)data.size();
}

// Handle peer request (when other peers want pieces from us)
void handle_peer_request(int client_fd) {
    char buffer[BUFFER_SIZE];
    
    while (running) {
        memset(buffer, 0, BUFFER_SIZE);
        int bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);
        
        if (bytes_read <= 0) break;
        
        string request(buffer);
        vector<string> tokens = tokenize(request);
        
        if (tokens.empty()) continue;
        
        if (tokens[0] == "GET_PIECE" && tokens.size() >= 4) {
            // GET_PIECE group_id filename piece_index
            string group_id = tokens[1];
            string filename = tokens[2];
            int piece_index = stoi(tokens[3]);
            
            string key = group_id + ":" + filename;
            
            lock_guard<mutex> lock(share_mutex);
            if (shared_files.find(key) != shared_files.end()) {
                SharedFile& sf = shared_files[key];
                if (piece_index < sf.num_pieces && sf.pieces_available[piece_index]) {
                    vector<char> piece_data;
                    if (get_piece_data(sf.filepath, piece_index, piece_data)) {
                        // Send piece: PIECE <piece_index> <size> <data>
                        string header = "PIECE " + to_string(piece_index) + " " + 
                                       to_string(piece_data.size()) + " ";
                        send(client_fd, header.c_str(), header.size(), 0);
                        send(client_fd, piece_data.data(), piece_data.size(), 0);
                        continue;
                    }
                }
            }
            // Check active downloads for leeching
            if (active_downloads.find(key) != active_downloads.end()) {
                DownloadInfo* di = active_downloads[key];
                if (piece_index < di->total_pieces && di->pieces_completed[piece_index]) {
                    vector<char> piece_data;
                    if (get_piece_data(di->dest_path, piece_index, piece_data)) {
                        string header = "PIECE " + to_string(piece_index) + " " + 
                                       to_string(piece_data.size()) + " ";
                        send(client_fd, header.c_str(), header.size(), 0);
                        send(client_fd, piece_data.data(), piece_data.size(), 0);
                        continue;
                    }
                }
            }
            
            string error = "ERROR:Piece not available";
            send(client_fd, error.c_str(), error.size(), 0);
        }
        else if (tokens[0] == "GET_PIECES_INFO" && tokens.size() >= 3) {
            // GET_PIECES_INFO group_id filename
            string group_id = tokens[1];
            string filename = tokens[2];
            string key = group_id + ":" + filename;
            
            string response = "PIECES_INFO:";
            
            lock_guard<mutex> lock(share_mutex);
            if (shared_files.find(key) != shared_files.end()) {
                SharedFile& sf = shared_files[key];
                for (int i = 0; i < sf.num_pieces; i++) {
                    response += sf.pieces_available[i] ? "1" : "0";
                }
            } else if (active_downloads.find(key) != active_downloads.end()) {
                DownloadInfo* di = active_downloads[key];
                for (int i = 0; i < di->total_pieces; i++) {
                    response += di->pieces_completed[i] ? "1" : "0";
                }
            } else {
                response = "ERROR:File not found";
            }
            
            send(client_fd, response.c_str(), response.size(), 0);
        }
    }
    
    close(client_fd);
}

// Peer server thread
void peer_server_thread() {
    peer_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (peer_server_fd < 0) {
        cerr << "Failed to create peer server socket" << endl;
        return;
    }
    
    int opt = 1;
    setsockopt(peer_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(my_port);
    
    if (bind(peer_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        cerr << "Failed to bind peer server on port " << my_port << endl;
        return;
    }
    
    if (listen(peer_server_fd, 50) < 0) {
        cerr << "Failed to listen on peer server" << endl;
        return;
    }
    
    while (running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_fd = accept(peer_server_fd, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (running) continue;
            break;
        }
        
        thread handler(handle_peer_request, client_fd);
        handler.detach();
    }
}

// Connect to peer
int connect_to_peer(const string& ip, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
    
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    
    return sock;
}

// Get piece availability from peer
vector<bool> get_peer_pieces(const string& ip, int port, const string& group_id, const string& filename) {
    vector<bool> pieces;
    
    int sock = connect_to_peer(ip, port);
    if (sock < 0) return pieces;
    
    string request = "GET_PIECES_INFO " + group_id + " " + filename;
    send(sock, request.c_str(), request.size(), 0);
    
    char buffer[BUFFER_SIZE];
    memset(buffer, 0, BUFFER_SIZE);
    int bytes_read = recv(sock, buffer, BUFFER_SIZE - 1, 0);
    close(sock);
    
    if (bytes_read <= 0) return pieces;
    
    string response(buffer);
    if (response.substr(0, 12) == "PIECES_INFO:") {
        string bits = response.substr(12);
        for (char c : bits) {
            pieces.push_back(c == '1');
        }
    }
    
    return pieces;
}

// Download piece from peer
bool download_piece_from_peer(const string& ip, int port, const string& group_id, 
                               const string& filename, int piece_index, 
                               vector<char>& piece_data, const string& expected_hash) {
    int sock = connect_to_peer(ip, port);
    if (sock < 0) return false;
    
    string request = "GET_PIECE " + group_id + " " + filename + " " + to_string(piece_index);
    send(sock, request.c_str(), request.size(), 0);
    
    // Receive header first
    char header_buf[256];
    memset(header_buf, 0, 256);
    int header_bytes = recv(sock, header_buf, 255, 0);
    
    if (header_bytes <= 0) {
        close(sock);
        return false;
    }
    
    string header(header_buf);
    if (header.substr(0, 5) == "ERROR") {
        close(sock);
        return false;
    }
    
    // Parse header: PIECE <piece_index> <size> <data...>
    vector<string> tokens = tokenize(header);
    if (tokens.size() < 3 || tokens[0] != "PIECE") {
        close(sock);
        return false;
    }
    
    int size = stoi(tokens[2]);
    
    // Find where data starts (after "PIECE X SIZE ")
    size_t data_start = header.find(tokens[2]) + tokens[2].size() + 1;
    
    // Copy initial data from header
    piece_data.clear();
    if (data_start < header.size()) {
        piece_data.insert(piece_data.end(), header_buf + data_start, header_buf + header_bytes);
    }
    
    // Receive remaining data
    while ((int)piece_data.size() < size) {
        char buf[BUFFER_SIZE];
        int remaining = size - piece_data.size();
        int to_read = min(remaining, BUFFER_SIZE);
        
        int bytes = recv(sock, buf, to_read, 0);
        if (bytes <= 0) break;
        
        piece_data.insert(piece_data.end(), buf, buf + bytes);
    }
    
    close(sock);
    
    if ((int)piece_data.size() != size) {
        return false;
    }
    
    // Verify hash
    if (!expected_hash.empty()) {
        string actual_hash = compute_sha1(piece_data.data(), piece_data.size());
        if (actual_hash.substr(0, 40) != expected_hash.substr(0, 40)) {
            return false;
        }
    }
    
    return true;
}

// Piece selection algorithm - Rarest First with some randomization
vector<pair<int, pair<string, int>>> select_pieces_to_download(
    DownloadInfo* di,
    const vector<pair<string, int>>& peers,
    const map<pair<string, int>, vector<bool>>& peer_pieces) {
    
    vector<pair<int, pair<string, int>>> result; // (piece_index, (ip, port))
    
    // Count availability of each piece
    map<int, vector<pair<string, int>>> piece_to_peers;
    
    for (const auto& peer : peers) {
        auto it = peer_pieces.find(peer);
        if (it == peer_pieces.end()) continue;
        
        const vector<bool>& pieces = it->second;
        for (int i = 0; i < (int)pieces.size() && i < di->total_pieces; i++) {
            if (pieces[i] && !di->pieces_completed[i]) {
                piece_to_peers[i].push_back(peer);
            }
        }
    }
    
    // Sort by rarity (fewer peers = more rare = higher priority)
    vector<pair<int, int>> piece_rarity; // (piece_index, num_peers)
    for (const auto& pp : piece_to_peers) {
        piece_rarity.push_back({pp.first, pp.second.size()});
    }
    
    sort(piece_rarity.begin(), piece_rarity.end(), 
         [](const auto& a, const auto& b) { return a.second < b.second; });
    
    // Assign pieces to peers, balancing load
    map<pair<string, int>, int> peer_load;
    
    for (const auto& pr : piece_rarity) {
        int piece_idx = pr.first;
        const auto& available_peers = piece_to_peers[piece_idx];
        
        if (available_peers.empty()) continue;
        
        // Find peer with lowest load
        pair<string, int> best_peer = available_peers[0];
        int min_load = peer_load[best_peer];
        
        for (const auto& peer : available_peers) {
            if (peer_load[peer] < min_load) {
                min_load = peer_load[peer];
                best_peer = peer;
            }
        }
        
        result.push_back({piece_idx, best_peer});
        peer_load[best_peer]++;
    }
    
    return result;
}

// Download worker thread
void download_worker(DownloadInfo* di, int piece_index, const string& ip, int port) {
    vector<char> piece_data;
    string expected_hash = (piece_index < (int)di->piece_hashes.size()) ? 
                           di->piece_hashes[piece_index] : "";
    
    bool success = download_piece_from_peer(ip, port, di->group_id, di->filename, 
                                            piece_index, piece_data, expected_hash);
    
    if (success) {
        lock_guard<mutex> lock(download_mutex);
        if (write_piece_data(di->dest_path, piece_index, piece_data)) {
            di->pieces_completed[piece_index] = true;
            
            // Check if download is complete
            bool all_complete = true;
            for (int i = 0; i < di->total_pieces; i++) {
                if (!di->pieces_completed[i]) {
                    all_complete = false;
                    break;
                }
            }
            
            if (all_complete) {
                di->is_complete = true;
                di->is_downloading = false;
            }
        }
    }
}

// Main download function
void download_file(const string& group_id, const string& filename, const string& dest_path) {
    // Get peer info from tracker
    string cmd = "get_peers " + group_id + " " + filename + " " + current_user;
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 5) == "ERROR") {
        lock_guard<mutex> lock(cout_mutex);
        cout << response.substr(6) << endl;
        return;
    }
    
    // Parse response: PEERS:filesize|sha1|piece_hashes|peer1,peer2,...
    if (response.substr(0, 6) != "PEERS:") {
        lock_guard<mutex> lock(cout_mutex);
        cout << "Invalid response from tracker" << endl;
        return;
    }
    
    string data = response.substr(6);
    vector<string> parts = tokenize(data, '|');
    
    if (parts.size() < 4) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "Invalid peer data" << endl;
        return;
    }
    
    long long filesize = stoll(parts[0]);
    string file_hash = parts[1];
    string piece_hashes_concat = parts[2];
    string peers_str = parts[3];
    
    if (peers_str == "NO_ACTIVE_PEERS") {
        lock_guard<mutex> lock(cout_mutex);
        cout << "No active peers for this file" << endl;
        return;
    }
    
    // Parse piece hashes
    vector<string> piece_hashes;
    for (size_t i = 0; i < piece_hashes_concat.length(); i += 40) {
        if (i + 40 <= piece_hashes_concat.length()) {
            piece_hashes.push_back(piece_hashes_concat.substr(i, 40));
        }
    }
    
    // Parse peers
    vector<pair<string, int>> peers;
    vector<string> peer_tokens = tokenize(peers_str, ',');
    for (const auto& pt : peer_tokens) {
        size_t colon = pt.find(':');
        if (colon != string::npos) {
            string ip = pt.substr(0, colon);
            int port = stoi(pt.substr(colon + 1));
            if (ip != my_ip || port != my_port) { // Don't include self
                peers.push_back({ip, port});
            }
        }
    }
    
    if (peers.empty()) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "No available peers for this file" << endl;
        return;
    }
    
    // Create download info
    string key = group_id + ":" + filename;
    DownloadInfo* di = new DownloadInfo();
    di->group_id = group_id;
    di->filename = filename;
    di->dest_path = dest_path;
    di->filesize = filesize;
    di->file_hash = file_hash;
    di->piece_hashes = piece_hashes;
    di->total_pieces = (filesize + PIECE_SIZE - 1) / PIECE_SIZE;
    di->pieces_completed.resize(di->total_pieces, false);
    di->is_downloading = true;
    
    {
        lock_guard<mutex> lock(download_mutex);
        active_downloads[key] = di;
    }
    
    // Create empty file
    int fd = open(dest_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        lock_guard<mutex> lock(cout_mutex);
        cout << "Failed to create destination file" << endl;
        delete di;
        return;
    }
    
    // Pre-allocate file
    if (ftruncate(fd, filesize) < 0) {
        close(fd);
        lock_guard<mutex> lock(cout_mutex);
        cout << "Failed to allocate file space" << endl;
        delete di;
        return;
    }
    close(fd);
    
    {
        lock_guard<mutex> lock(cout_mutex);
        cout << "Starting download: " << filename << " (" << filesize << " bytes, " 
             << di->total_pieces << " pieces)" << endl;
    }
    
    // Get piece availability from all peers
    map<pair<string, int>, vector<bool>> peer_pieces;
    for (const auto& peer : peers) {
        vector<bool> pieces = get_peer_pieces(peer.first, peer.second, group_id, filename);
        if (!pieces.empty()) {
            peer_pieces[peer] = pieces;
        }
    }
    
    // Download loop
    while (!di->is_complete && running) {
        // Select pieces to download
        auto selections = select_pieces_to_download(di, peers, peer_pieces);
        
        if (selections.empty()) {
            // Refresh peer info
            this_thread::sleep_for(chrono::seconds(2));
            for (const auto& peer : peers) {
                vector<bool> pieces = get_peer_pieces(peer.first, peer.second, group_id, filename);
                if (!pieces.empty()) {
                    peer_pieces[peer] = pieces;
                }
            }
            continue;
        }
        
        // Download pieces in parallel (up to 4 at a time)
        vector<thread> workers;
        int batch_size = min(4, (int)selections.size());
        
        for (int i = 0; i < batch_size; i++) {
            auto& sel = selections[i];
            workers.emplace_back(download_worker, di, sel.first, 
                               sel.second.first, sel.second.second);
        }
        
        for (auto& w : workers) {
            w.join();
        }
        
        // Small delay between batches
        this_thread::sleep_for(chrono::milliseconds(100));
    }
    
    if (di->is_complete) {
        // Verify complete file hash
        string actual_hash = compute_file_sha1(dest_path);
        
        if (!file_hash.empty() && actual_hash.substr(0, 40) != file_hash.substr(0, 40)) {
            lock_guard<mutex> lock(cout_mutex);
            cout << "WARNING: File hash mismatch! Download may be corrupted." << endl;
        }
        
        // Register as seeder
        string add_cmd = "add_seeder " + group_id + " " + filename + " " + current_user;
        send_to_tracker(add_cmd);
        
        // Add to shared files
        {
            lock_guard<mutex> lock(share_mutex);
            SharedFile sf;
            sf.group_id = group_id;
            sf.filename = filename;
            sf.filepath = dest_path;
            sf.filesize = filesize;
            sf.file_hash = file_hash;
            sf.piece_hashes = piece_hashes;
            sf.num_pieces = di->total_pieces;
            sf.pieces_available.resize(di->total_pieces, true);
            shared_files[key] = sf;
        }
        
        {
            lock_guard<mutex> lock(cout_mutex);
            cout << "Download complete: " << filename << endl;
        }
    }
}

// Handle create_user
void cmd_create_user(const vector<string>& tokens) {
    if (tokens.size() < 3) {
        cout << "Usage: create_user <user_id> <password>" << endl;
        return;
    }
    
    string cmd = "create_user " + tokens[1] + " " + tokens[2];
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 7) == "SUCCESS") {
        cout << "User created successfully" << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle login
void cmd_login(const vector<string>& tokens) {
    if (tokens.size() < 3) {
        cout << "Usage: login <user_id> <password>" << endl;
        return;
    }
    
    if (is_logged_in) {
        cout << "Already logged in as " << current_user << endl;
        return;
    }
    
    string cmd = "login " + tokens[1] + " " + tokens[2] + " " + to_string(my_port);
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 7) == "SUCCESS") {
        current_user = tokens[1];
        is_logged_in = true;
        cout << "Login successful" << endl;
        
        // Re-register shared files
        lock_guard<mutex> lock(share_mutex);
        for (auto& sf : shared_files) {
            string upload_cmd = "upload_file " + sf.second.group_id + " " + sf.second.filename + 
                               " " + to_string(sf.second.filesize) + " " + sf.second.file_hash + " ";
            
            string piece_hashes_str = "";
            for (const auto& ph : sf.second.piece_hashes) {
                piece_hashes_str += ph;
            }
            if (piece_hashes_str.empty()) piece_hashes_str = "NONE";
            
            upload_cmd += piece_hashes_str + " " + current_user;
            send_to_tracker(upload_cmd);
        }
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle logout
void cmd_logout() {
    if (!is_logged_in) {
        cout << "Not logged in" << endl;
        return;
    }
    
    string cmd = "logout " + current_user;
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 7) == "SUCCESS") {
        current_user = "";
        is_logged_in = false;
        cout << "Logout successful" << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle create_group
void cmd_create_group(const vector<string>& tokens) {
    if (!is_logged_in) {
        cout << "Please login first" << endl;
        return;
    }
    
    if (tokens.size() < 2) {
        cout << "Usage: create_group <group_id>" << endl;
        return;
    }
    
    string cmd = "create_group " + tokens[1] + " " + current_user;
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 7) == "SUCCESS") {
        cout << "Group created successfully" << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle join_group
void cmd_join_group(const vector<string>& tokens) {
    if (!is_logged_in) {
        cout << "Please login first" << endl;
        return;
    }
    
    if (tokens.size() < 2) {
        cout << "Usage: join_group <group_id>" << endl;
        return;
    }
    
    string cmd = "join_group " + tokens[1] + " " + current_user;
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 7) == "SUCCESS") {
        cout << "Join request sent" << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle leave_group
void cmd_leave_group(const vector<string>& tokens) {
    if (!is_logged_in) {
        cout << "Please login first" << endl;
        return;
    }
    
    if (tokens.size() < 2) {
        cout << "Usage: leave_group <group_id>" << endl;
        return;
    }
    
    string cmd = "leave_group " + tokens[1] + " " + current_user;
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 7) == "SUCCESS") {
        cout << "Left group successfully" << endl;
        
        // Remove shared files for this group
        lock_guard<mutex> lock(share_mutex);
        vector<string> to_remove;
        for (auto& sf : shared_files) {
            if (sf.second.group_id == tokens[1]) {
                to_remove.push_back(sf.first);
            }
        }
        for (const auto& key : to_remove) {
            shared_files.erase(key);
        }
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle list_requests
void cmd_list_requests(const vector<string>& tokens) {
    if (!is_logged_in) {
        cout << "Please login first" << endl;
        return;
    }
    
    if (tokens.size() < 2) {
        cout << "Usage: list_requests <group_id>" << endl;
        return;
    }
    
    string cmd = "list_requests " + tokens[1] + " " + current_user;
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 9) == "REQUESTS:") {
        cout << "Pending requests: " << response.substr(9) << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle accept_request
void cmd_accept_request(const vector<string>& tokens) {
    if (!is_logged_in) {
        cout << "Please login first" << endl;
        return;
    }
    
    if (tokens.size() < 3) {
        cout << "Usage: accept_request <group_id> <user_id>" << endl;
        return;
    }
    
    string cmd = "accept_request " + tokens[1] + " " + tokens[2] + " " + current_user;
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 7) == "SUCCESS") {
        cout << "Request accepted" << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle list_groups
void cmd_list_groups() {
    string cmd = "list_groups";
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 7) == "GROUPS:") {
        cout << "Groups: " << response.substr(7) << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle list_files
void cmd_list_files(const vector<string>& tokens) {
    if (!is_logged_in) {
        cout << "Please login first" << endl;
        return;
    }
    
    if (tokens.size() < 2) {
        cout << "Usage: list_files <group_id>" << endl;
        return;
    }
    
    string cmd = "list_files " + tokens[1] + " " + current_user;
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 6) == "FILES:") {
        cout << "Files: " << response.substr(6) << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle upload_file
void cmd_upload_file(const vector<string>& tokens) {
    if (!is_logged_in) {
        cout << "Please login first" << endl;
        return;
    }
    
    if (tokens.size() < 3) {
        cout << "Usage: upload_file <file_path> <group_id>" << endl;
        return;
    }
    
    string filepath = tokens[1];
    string group_id = tokens[2];
    
    // Check if file exists
    long long filesize = get_file_size(filepath);
    if (filesize < 0) {
        cout << "File not found: " << filepath << endl;
        return;
    }
    
    // Extract filename from path
    string filename = filepath;
    size_t slash = filepath.rfind('/');
    if (slash != string::npos) {
        filename = filepath.substr(slash + 1);
    }
    
    cout << "Computing file hashes..." << endl;
    
    // Compute SHA1 hash of complete file
    string file_hash = compute_file_sha1(filepath);
    
    // Compute piece-wise hashes
    vector<string> piece_hashes = compute_piece_hashes(filepath, filesize);
    
    // Concatenate piece hashes
    string piece_hashes_str = "";
    for (const auto& ph : piece_hashes) {
        piece_hashes_str += ph;
    }
    if (piece_hashes_str.empty()) piece_hashes_str = "NONE";
    
    // Send to tracker
    string cmd = "upload_file " + group_id + " " + filename + " " + to_string(filesize) + 
                 " " + file_hash + " " + piece_hashes_str + " " + current_user;
    string response = send_to_tracker(cmd);
    
    if (response.substr(0, 7) == "SUCCESS") {
        // Add to shared files
        string key = group_id + ":" + filename;
        {
            lock_guard<mutex> lock(share_mutex);
            SharedFile sf;
            sf.group_id = group_id;
            sf.filename = filename;
            sf.filepath = filepath;
            sf.filesize = filesize;
            sf.file_hash = file_hash;
            sf.piece_hashes = piece_hashes;
            sf.num_pieces = (filesize + PIECE_SIZE - 1) / PIECE_SIZE;
            sf.pieces_available.resize(sf.num_pieces, true);
            shared_files[key] = sf;
        }
        
        cout << "File uploaded successfully" << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Handle download_file
void cmd_download_file(const vector<string>& tokens) {
    if (!is_logged_in) {
        cout << "Please login first" << endl;
        return;
    }
    
    if (tokens.size() < 4) {
        cout << "Usage: download_file <group_id> <file_name> <destination_path>" << endl;
        return;
    }
    
    string group_id = tokens[1];
    string filename = tokens[2];
    string dest_path = tokens[3];
    
    // Start download in a separate thread
    thread download_thread(download_file, group_id, filename, dest_path);
    download_thread.detach();
    
    cout << "Download started in background" << endl;
}

// Handle show_downloads
void cmd_show_downloads() {
    lock_guard<mutex> lock(download_mutex);
    
    if (active_downloads.empty()) {
        cout << "No active downloads" << endl;
        return;
    }
    
    for (const auto& dl : active_downloads) {
        DownloadInfo* di = dl.second;
        
        int completed = 0;
        for (int i = 0; i < di->total_pieces; i++) {
            if (di->pieces_completed[i]) completed++;
        }
        
        if (di->is_complete) {
            cout << "[C] [" << di->group_id << "] " << di->filename << endl;
        } else {
            cout << "[D] [" << di->group_id << "] " << di->filename 
                 << " (" << completed << "/" << di->total_pieces << " pieces)" << endl;
        }
    }
}

// Handle stop_share
void cmd_stop_share(const vector<string>& tokens) {
    if (!is_logged_in) {
        cout << "Please login first" << endl;
        return;
    }
    
    if (tokens.size() < 3) {
        cout << "Usage: stop_share <group_id> <file_name>" << endl;
        return;
    }
    
    string group_id = tokens[1];
    string filename = tokens[2];
    string key = group_id + ":" + filename;
    
    // Notify tracker
    string cmd = "stop_share " + group_id + " " + filename + " " + current_user;
    string response = send_to_tracker(cmd);
    
    // Remove from local shared files
    {
        lock_guard<mutex> lock(share_mutex);
        shared_files.erase(key);
    }
    
    if (response.substr(0, 7) == "SUCCESS") {
        cout << "Stopped sharing: " << filename << endl;
    } else {
        cout << response.substr(6) << endl;
    }
}

// Signal handler
void signal_handler(int /* sig */) {
    running = false;
    if (peer_server_fd >= 0) {
        close(peer_server_fd);
    }
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cerr << "Usage: ./client <IP>:<PORT> tracker_info.txt" << endl;
        return 1;
    }
    
    // Parse IP:PORT
    string addr = argv[1];
    size_t colon = addr.find(':');
    if (colon == string::npos) {
        cerr << "Invalid address format. Use IP:PORT" << endl;
        return 1;
    }
    
    my_ip = addr.substr(0, colon);
    my_port = stoi(addr.substr(colon + 1));
    
    // Parse tracker info
    trackers = parse_tracker_info(argv[2]);
    if (trackers.empty()) {
        cerr << "No trackers found in " << argv[2] << endl;
        return 1;
    }
    
    // Setup signal handler
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Start peer server
    thread peer_thread(peer_server_thread);
    peer_thread.detach();
    
    cout << "Client started on " << my_ip << ":" << my_port << endl;
    cout << "Connected to " << trackers.size() << " tracker(s)" << endl;
    
    // Main command loop
    string line;
    while (running) {
        cout << "> ";
        if (!getline(cin, line)) break;
        
        if (line.empty()) continue;
        
        vector<string> tokens = tokenize(line);
        if (tokens.empty()) continue;
        
        string cmd = tokens[0];
        
        if (cmd == "create_user") {
            cmd_create_user(tokens);
        }
        else if (cmd == "login") {
            cmd_login(tokens);
        }
        else if (cmd == "logout") {
            cmd_logout();
        }
        else if (cmd == "create_group") {
            cmd_create_group(tokens);
        }
        else if (cmd == "join_group") {
            cmd_join_group(tokens);
        }
        else if (cmd == "leave_group") {
            cmd_leave_group(tokens);
        }
        else if (cmd == "list_requests") {
            cmd_list_requests(tokens);
        }
        else if (cmd == "accept_request") {
            cmd_accept_request(tokens);
        }
        else if (cmd == "list_groups") {
            cmd_list_groups();
        }
        else if (cmd == "list_files") {
            cmd_list_files(tokens);
        }
        else if (cmd == "upload_file") {
            cmd_upload_file(tokens);
        }
        else if (cmd == "download_file") {
            cmd_download_file(tokens);
        }
        else if (cmd == "show_downloads") {
            cmd_show_downloads();
        }
        else if (cmd == "stop_share") {
            cmd_stop_share(tokens);
        }
        else if (cmd == "quit" || cmd == "exit") {
            if (is_logged_in) {
                cmd_logout();
            }
            running = false;
            break;
        }
        else {
            cout << "Unknown command: " << cmd << endl;
            cout << "Available commands:" << endl;
            cout << "  create_user <user_id> <password>" << endl;
            cout << "  login <user_id> <password>" << endl;
            cout << "  logout" << endl;
            cout << "  create_group <group_id>" << endl;
            cout << "  join_group <group_id>" << endl;
            cout << "  leave_group <group_id>" << endl;
            cout << "  list_requests <group_id>" << endl;
            cout << "  accept_request <group_id> <user_id>" << endl;
            cout << "  list_groups" << endl;
            cout << "  list_files <group_id>" << endl;
            cout << "  upload_file <file_path> <group_id>" << endl;
            cout << "  download_file <group_id> <file_name> <destination_path>" << endl;
            cout << "  show_downloads" << endl;
            cout << "  stop_share <group_id> <file_name>" << endl;
            cout << "  quit" << endl;
        }
    }
    
    cout << "Client shutting down..." << endl;
    
    // Cleanup
    if (peer_server_fd >= 0) {
        close(peer_server_fd);
    }
    
    // Cleanup downloads
    for (auto& dl : active_downloads) {
        delete dl.second;
    }
    
    return 0;
}
