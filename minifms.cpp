#include <iostream>
#include <vector>
#include <string>
#include <ctime>
#include <fstream>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <map>
#include <sstream>
#include <algorithm>
#include <queue>
#include <iomanip>
#include <cstring>
#include <chrono>
#include <memory>
#include <atomic>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif

using namespace std;

// 系统常量定义
#define VERSION "2.0"
#define MAX_USERS 100
#define MAX_FCBS 10000
#define MAX_FILENAME_LEN 64
#define MAX_BLOCKS 9216 // 最大块数

// 用户结构体
struct User
{
    int isused = 0;
    char username[32];
    char password[32];
    bool locked = false;
    int loginFailCount = 0;
    int rootDirId = 0;
    bool isActive = false;
    time_t createTime;
    int userId;

    User()
    {
        memset(username, 0, sizeof(username));
        memset(password, 0, sizeof(password));
        createTime = time(nullptr);
    }
};

// 文件控制块FCB
struct FCB
{
    int isused = 0;
    char name[MAX_FILENAME_LEN];
    int type = 0; // 0=文件，1=目录
    int owner = 0;
    size_t size = 0;
    int address = 0;
    time_t createTime;
    time_t modifyTime;
    time_t accessTime;
    bool locked = false;
    int lockOwner = -1;
    int parentDir = -1;

    FCB()
    {
        memset(name, 0, sizeof(name));
        createTime = modifyTime = accessTime = time(nullptr);
    }
};

// 文件描述符结构（避免与Windows OpenFile冲突）
struct FileDesc
{
    int fcbId = -1;
    int userId = -1;
    size_t position = 0;
    int mode = 0;
    bool isOpen = false;

    FileDesc() = default;
    FileDesc(int fid, int uid, int m) : fcbId(fid), userId(uid), mode(m), isOpen(true) {}
};

// 简化的共享数据结构
struct SharedData
{
    atomic<int> modifyCount{0};
    atomic<int> nextUserId{1};
    atomic<int> nextFcbId{1};
    User users[MAX_USERS];
    FCB fcbs[MAX_FCBS];
    char fileContents[MAX_FCBS][4096];
    bool initialized = false;

    SharedData()
    {
        for (int i = 0; i < MAX_FCBS; ++i)
        {
            memset(fileContents[i], 0, sizeof(fileContents[i]));
        }
    }
};

// 会话结构体
struct Session
{
    User *user = nullptr;
    int currentDirId = 0;
    bool active = false;
    vector<FileDesc> openFiles;

    int addOpenFile(int fcbId, int mode)
    {
        for (size_t i = 0; i < openFiles.size(); ++i)
        {
            if (!openFiles[i].isOpen)
            {
                openFiles[i] = FileDesc(fcbId, user->userId, mode);
                return static_cast<int>(i);
            }
        }
        openFiles.push_back(FileDesc(fcbId, user->userId, mode));
        return static_cast<int>(openFiles.size() - 1);
    }

    bool closeFile(int fd)
    {
        if (fd >= 0 && fd < static_cast<int>(openFiles.size()) && openFiles[fd].isOpen)
        {
            openFiles[fd].isOpen = false;
            return true;
        }
        return false;
    }
};

// 命令请求结构体
struct CommandRequest
{
    Session *session;
    string commandLine;

    CommandRequest() = default;
    CommandRequest(Session *s, const string &cmd) : session(s), commandLine(cmd) {}
};

// MiniFMS主类
class MiniFMS
{
private:
    SharedData *sharedData = nullptr;
    atomic<bool> systemRunning{true};
    atomic<bool> shouldExit{false};

#ifdef _WIN32
    HANDLE hMapFile = nullptr;
#else
    int shmFd = -1;
#endif

    queue<CommandRequest> commandQueue;
    mutex queueMutex;
    condition_variable queueCv;
    mutex diskMutex;
    bool ready = false;

    // 持久化相关变量
    const string DATA_FILE = "filesystem.dat";
    atomic<bool> dataChanged{false};
    thread autoSaveThreadHandle;
    thread diskMaintenanceThreadHandle;

    Session currentSession;

    int *fatBlock = nullptr;
    int *bitMap = nullptr;

    bool checkFileAccess(Session *session, int fileId, bool needWrite)
    {
        if (!session || !sharedData || fileId < 0 || fileId >= MAX_FCBS)
            return false;

        FCB &fcb = sharedData->fcbs[fileId];

        // 检查文件是否被锁定
        if (fcb.locked)
        {
            // 如果是写操作，任何用户都不能修改
            if (needWrite)
            {
                cout << " 错误：文件已被锁定，处于只读状态" << endl;
                cout << " - 当前锁定者：";
                for (int i = 0; i < MAX_USERS; i++)
                {
                    if (sharedData->users[i].isused &&
                        sharedData->users[i].userId == fcb.lockOwner)
                    {
                        cout << sharedData->users[i].username << endl;
                        break;
                    }
                }
                return false;
            }
        }
        return true;
    }

    // 清理资源
    void cleanup()
    {
        // 设置退出标记
        shouldExit = true;
        systemRunning = false;

        // 通知所有等待的线程
        queueCv.notify_all();

        // 等待自动保存线程和磁盘维护线程结束
        if (autoSaveThreadHandle.joinable())
        {
            autoSaveThreadHandle.join();
        }
        if (diskMaintenanceThreadHandle.joinable())
        {
            diskMaintenanceThreadHandle.join();
        }

        // 在退出前保存数据
        if (sharedData && sharedData->initialized)
        {
            saveDataToDisk(false);
        }

        // 清理共享数据
        delete sharedData;
        sharedData = nullptr;

        // 清理 FAT 表和位图
        delete[] fatBlock;
        delete[] bitMap;
    }

public:
    MiniFMS();  // 构造函数
    ~MiniFMS(); // 析构函数

    // 用户管理
    bool registerUser(const string &username, const string &password); // 注册用户
    User *loginUser(const string &username, const string &password);   // 登录用户
    bool checkUserConflict(const string &username);                    // 检查用户名是否冲突

    // 文件管理
    int findFCB(int parentDir, const string &name);
    int createFCB(const string &name, int type, int owner, int parentDir);
    string getCurrentPath(int fcbId, int userId);
    string formatTime(time_t t);

    // 文件操作
    void createFile(Session *session, const string &fileName);
    void deleteFile(Session *session, const string &fileName);
    void listDirectory(Session *session);
    void showTree(Session *session);
    void showTreeRecursive(int fcbId, int depth, int userId);
    void showFileHead(Session *session, const string &fileName, int numLines);
    void showFileTail(Session *session, const string &fileName, int numLines);

    // 用户交互
    void showWelcome();
    void showHelp();
    void userInteractionThread(Session &session); // 用户交互线程
    void diskMaintenanceThread();                 // 磁盘维护线程
    void processCommand(CommandRequest &req);     // 处理命令
    void run();                                   // 运行系统

    // 持久化功能
    bool saveDataToDisk(bool silent = false); // 保存数据到磁盘
    bool loadDataFromDisk();                  // 从磁盘加载数据
    void autoSaveThread();                    // 自动保存线程

    void findAllFiles(vector<int> &files, int fcbId);
    void deleteFCB(int fcbId);

    // 导入外部文件到文件系统
    bool importFile(Session *session, const string &externalPath, const string &internalName);
    // 导出文件到外部文件系统
    bool exportFile(Session *session, const string &internalName, const string &externalPath);

    // 通过路径查找FCB
    int findFCBByPath(Session *session, const string &path)
    {
        if (!session || !sharedData)
            return -1;
        if (path.empty())
            return -1;

        // 处理特殊路径
        if (path == "/")
        {
            return session->user->rootDirId; // 返回用户的根目录
        }

        // 确定起始目录
        int currentDir = session->currentDirId;

        // 分割路径
        vector<string> parts;
        stringstream ss(path);
        string part;

        // 检查是否是绝对路径
        bool isAbsolutePath = !path.empty() && path[0] == '/';
        if (isAbsolutePath)
        {
            currentDir = session->user->rootDirId;
        }

        // 使用/分割路径
        while (getline(ss, part, '/'))
        {
            if (part.empty() || part == ".")
                continue; // 跳过空部分和当前目录符号
            parts.push_back(part);
        }

        if (parts.empty())
            return currentDir;

        // 遍历路径的每一部分
        for (const string &dirName : parts)
        {
            if (dirName == "..")
            {
                // 返回上级目录
                if (currentDir == session->user->rootDirId)
                {
                    // 已经在根目录，无法再往上
                    continue;
                }
                FCB &currentFCB = sharedData->fcbs[currentDir];
                currentDir = currentFCB.parentDir;
                if (currentDir == -1)
                {
                    return -1; // 无效的父目录
                }
            }
            else
            {
                int nextId = findFCB(currentDir, dirName);
                if (nextId == -1)
                {
                    return -1; // 路径中的某个部分不存在
                }
                currentDir = nextId;
            }
        }

        return currentDir;
    }
};

// 构造函数和析构函数定义
MiniFMS::MiniFMS()
{
    // 初始化共享数据
    sharedData = new SharedData();

    // 初始化 FAT 表和位图
    fatBlock = new int[MAX_BLOCKS];
    bitMap = new int[MAX_BLOCKS];
    memset(fatBlock, 0, sizeof(int) * MAX_BLOCKS);
    memset(bitMap, 0, sizeof(int) * MAX_BLOCKS);

    // 尝试从磁盘加载数据
    if (!loadDataFromDisk())
    {
        cout << "初始化新的文件系统..." << endl;
        // 初始化根目录
        FCB &rootFcb = sharedData->fcbs[0];
        rootFcb.isused = 1;
        strcpy(rootFcb.name, "/");
        rootFcb.type = 1;
        rootFcb.owner = 0;
        rootFcb.createTime = rootFcb.modifyTime = rootFcb.accessTime = time(nullptr);
        rootFcb.parentDir = -1;
        sharedData->nextFcbId = 1;
        sharedData->initialized = true;

        // 为根目录分配第一个数据块
        bitMap[0] = 1;
        fatBlock[0] = -1; // 标记为链表结束
    }
    else
    {
        cout << "从磁盘加载文件系统数据成功!" << endl;
    }
}

MiniFMS::~MiniFMS()
{
    // 清理资源
    delete[] fatBlock;
    delete[] bitMap;
    cleanup();
}

int MiniFMS::findFCB(int parentDir, const string &name)
{
    if (!sharedData || parentDir < 0 || parentDir >= MAX_FCBS)
        return -1;

    for (int i = 0; i < MAX_FCBS; ++i)
    {
        if (sharedData->fcbs[i].isused &&
            sharedData->fcbs[i].parentDir == parentDir &&
            strcmp(sharedData->fcbs[i].name, name.c_str()) == 0)
        {
            return i;
        }
    }
    return -1;
}

int MiniFMS::createFCB(const string &name, int type, int owner, int parentDir)
{
    if (!sharedData)
        return -1;

    lock_guard<mutex> lock(diskMutex);

    int fcbId = -1;
    for (int i = sharedData->nextFcbId; i < MAX_FCBS; ++i)
    {
        if (!sharedData->fcbs[i].isused)
        {
            fcbId = i;
            break;
        }
    }

    if (fcbId == -1)
        return -1;

    FCB &fcb = sharedData->fcbs[fcbId];
    fcb.isused = 1;
    strncpy(fcb.name, name.c_str(), MAX_FILENAME_LEN - 1);
    fcb.type = type;
    fcb.owner = owner;
    fcb.size = 0;
    fcb.createTime = fcb.modifyTime = fcb.accessTime = time(nullptr);
    fcb.locked = false;
    fcb.lockOwner = -1;
    fcb.parentDir = parentDir;
    fcb.address = fcbId;

    if (type == 0)
    {
        memset(sharedData->fileContents[fcbId], 0, sizeof(sharedData->fileContents[fcbId]));
    }

    sharedData->nextFcbId = fcbId + 1;
    sharedData->modifyCount++;
    dataChanged = true;

    return fcbId;
}

bool MiniFMS::registerUser(const string &username, const string &password)
{
    if (!sharedData)
        return false;

    if (checkUserConflict(username))
    {
        cout << "用户名已存在!" << endl;
        return false;
    }

    lock_guard<mutex> lock(diskMutex);

    int userId = -1;
    for (int i = 0; i < MAX_USERS; i++)
    {
        if (!sharedData->users[i].isused)
        {
            userId = i;
            break;
        }
    }

    if (userId == -1)
    {
        cout << "用户数量已达上限!" << endl;
        return false;
    }

    User &user = sharedData->users[userId];
    user.isused = 1;
    user.userId = sharedData->nextUserId++;
    strncpy(user.username, username.c_str(), sizeof(user.username) - 1);
    strncpy(user.password, password.c_str(), sizeof(user.password) - 1);
    user.locked = false;
    user.loginFailCount = 0;
    user.createTime = time(nullptr);

    int rootDirId = createFCB(username, 1, user.userId, 0);
    if (rootDirId == -1)
    {
        user.isused = 0;
        cout << "创建用户目录失败!" << endl;
        return false;
    }

    user.rootDirId = rootDirId;
    cout << "用户注册成功!" << endl;

    // 立即保存数据到磁盘
    sharedData->modifyCount++;
    dataChanged = true;
    if (saveDataToDisk(true))
    {
        cout << "用户数据已保存到磁盘" << endl;
    }
    else
    {
        cout << "警告：用户数据保存失败，请尽快手动保存！" << endl;
    }

    return true;
}

bool MiniFMS::checkUserConflict(const string &username)
{
    if (!sharedData)
        return false;

    for (int i = 0; i < MAX_USERS; ++i)
    {
        if (sharedData->users[i].isused &&
            strcmp(sharedData->users[i].username, username.c_str()) == 0)
        {
            return true;
        }
    }
    return false;
}

User *MiniFMS::loginUser(const string &username, const string &password)
{
    if (!sharedData)
        return nullptr;

    for (int i = 0; i < MAX_USERS; ++i)
    {
        User &user = sharedData->users[i];
        if (user.isused && strcmp(user.username, username.c_str()) == 0)
        {
            if (user.locked)
            {
                cout << "账号已锁定!" << endl;
                return nullptr;
            }

            if (strcmp(user.password, password.c_str()) == 0)
            {
                user.loginFailCount = 0;
                user.isActive = true;
                cout << "登录成功!" << endl;
                showWelcome();
                return &user;
            }
            else
            {
                user.loginFailCount++;
                cout << "密码错误!" << endl;
                if (user.loginFailCount >= 3)
                {
                    user.locked = true;
                    cout << "账号已锁定!" << endl;
                }
                return nullptr;
            }
        }
    }

    cout << "用户不存在!" << endl;
    return nullptr;
}

string MiniFMS::getCurrentPath(int fcbId, int userId)
{
    if (!sharedData || fcbId <= 0 || fcbId >= MAX_FCBS)
        return "/";

    vector<string> pathParts;
    int current = fcbId;

    int userRootId = 0;
    for (int i = 0; i < MAX_USERS; ++i)
    {
        if (sharedData->users[i].isused && sharedData->users[i].userId == userId)
        {
            userRootId = sharedData->users[i].rootDirId;
            break;
        }
    }

    while (current != 0 && current != userRootId && current != -1)
    {
        if (current >= MAX_FCBS || !sharedData->fcbs[current].isused)
            break;
        pathParts.push_back(string(sharedData->fcbs[current].name));
        current = sharedData->fcbs[current].parentDir;
    }

    if (pathParts.empty())
    {
        return "/";
    }

    string path = "/";
    for (auto it = pathParts.rbegin(); it != pathParts.rend(); ++it)
    {
        path += *it;
        if (it + 1 != pathParts.rend())
        {
            path += "/";
        }
    }
    return path;
}

string MiniFMS::formatTime(time_t t)
{
    char buffer[80];
    struct tm *timeinfo = localtime(&t);
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    return string(buffer);
}

void MiniFMS::showWelcome()
{
    cout << "\n╔══════════════════════════════════════╗" << endl;
    cout << "║        Welcome to MiniFMS " << VERSION << "         ║" << endl;
    cout << "║     Advanced File System Pro        ║" << endl;
    cout << "║     Type 'help' for commands        ║" << endl;
    cout << "╚══════════════════════════════════════╝" << endl
         << endl;
}

void MiniFMS::showHelp()
{
    cout << "\n═══════════════ 命令列表 ═══════════════\n"
         << endl;
    cout << " 目录操作:" << endl;
    cout << "  cd [目录名]          切换目录 (.. 返回上级)" << endl;
    cout << "  dir                 显示当前目录内容" << endl;
    cout << "  mkdir [目录名]       创建目录" << endl;
    cout << "  rmdir [目录名]       删除空目录" << endl;

    cout << "\n 文件操作:" << endl;
    cout << "  create [文件名]      创建文件" << endl;
    cout << "  delete [文件名]      删除文件" << endl;
    cout << "  open [文件名] [模式] 打开文件 (r/w/rw)" << endl;
    cout << "  close [文件描述符]   关闭文件" << endl;
    cout << "  read [文件描述符]    读取文件" << endl;
    cout << "  write [文件描述符]   写入文件" << endl;
    cout << "  copy [源] [目标]     复制文件" << endl;
    cout << "  move [源] [目标]     移动文件" << endl;
    cout << "  flock [文件名]       加锁/解锁文件" << endl;
    cout << "  head -num [文件名]   显示文件前num行" << endl;
    cout << "  tail -num [文件名]   显示文件后num行" << endl;
    cout << "  lseek [文件描述符] [偏移量] 移动文件指针" << endl;

    cout << "\n 导入导出:" << endl;
    cout << "  import [外部路径] [系统内文件名]  导入外部文件" << endl;
    cout << "  export [系统内文件名] [外部路径]  导出文件到外部" << endl;

    cout << "\n 系统功能:" << endl;
    cout << "  tree                显示目录树" << endl;
    cout << "  save                手动保存数据到磁盘" << endl;
    cout << "  help                显示本帮助" << endl;
    cout << "  exit                退出系统" << endl;
    cout << "\n═══════════════════════════════════════\n"
         << endl;
}

void MiniFMS::createFile(Session *session, const string &fileName)
{
    if (!session || !sharedData)
        return;

    if (findFCB(session->currentDirId, fileName) != -1)
    {
        cout << "文件已存在: " << fileName << endl;
        return;
    }

    int newFileId = createFCB(fileName, 0, session->user->userId, session->currentDirId);
    if (newFileId != -1)
    {
        cout << "文件创建成功: " << fileName << endl;
    }
    else
    {
        cout << "文件创建失败" << endl;
    }
}

void MiniFMS::deleteFile(Session *session, const string &fileName)
{
    if (!session || !sharedData)
        return;

    int fileId = findFCB(session->currentDirId, fileName);
    if (fileId == -1 || sharedData->fcbs[fileId].type != 0)
    {
        cout << "文件不存在: " << fileName << endl;
        return;
    }

    // 检查文件访问权限
    if (!checkFileAccess(session, fileId, true))
    {
        return;
    }

    // 检查文件是否被打开
    for (const auto &openFile : session->openFiles)
    {
        if (openFile.isOpen && openFile.fcbId == fileId)
        {
            cout << " 错误：文件正在使用中，请先关闭文件" << endl;
            return;
        }
    }

    sharedData->fcbs[fileId].isused = 0;
    memset(sharedData->fileContents[fileId], 0, sizeof(sharedData->fileContents[fileId]));

    cout << "文件删除成功: " << fileName << endl;
    sharedData->modifyCount++;
    dataChanged = true;
}

void MiniFMS::listDirectory(Session *session)
{
    if (!session || !sharedData)
        return;

    cout << "\n目录内容 - " << getCurrentPath(session->currentDirId, session->user->userId) << "\n"
         << endl;
    cout << "类型\t名称\t\t大小\t\t修改时间" << endl;
    cout << "────────────────────────────────────────────────────────" << endl;

    bool hasContent = false;
    for (int i = 0; i < MAX_FCBS; ++i)
    {
        if (sharedData->fcbs[i].isused &&
            sharedData->fcbs[i].parentDir == session->currentDirId)
        {
            hasContent = true;

            string type = sharedData->fcbs[i].type == 1 ? "DIR" : "FILE";
            string name = string(sharedData->fcbs[i].name);
            string size = sharedData->fcbs[i].type == 1 ? "<DIR>" : to_string(sharedData->fcbs[i].size) + " bytes";
            string mtime = formatTime(sharedData->fcbs[i].modifyTime);

            cout << type << "\t" << setw(15) << left << name << "\t"
                 << setw(12) << left << size << "\t" << mtime << endl;
        }
    }

    if (!hasContent)
    {
        cout << "目录为空" << endl;
    }
    cout << endl;
}

void MiniFMS::showTree(Session *session)
{
    if (!session || !sharedData)
        return;

    cout << "\n 目录树结构\n"
         << endl;
    showTreeRecursive(session->currentDirId, 0, session->user->userId);
    cout << endl;
}

void MiniFMS::showTreeRecursive(int fcbId, int depth, int userId)
{
    if (!sharedData || fcbId < 0 || fcbId >= MAX_FCBS || !sharedData->fcbs[fcbId].isused)
        return;

    for (int i = 0; i < depth; ++i)
    {
        cout << "│   ";
    }

    FCB &fcb = sharedData->fcbs[fcbId];
    if (fcb.type == 1)
    {
        cout << "├──" << fcb.name << "/" << endl;

        for (int i = 0; i < MAX_FCBS; ++i)
        {
            if (sharedData->fcbs[i].isused && sharedData->fcbs[i].parentDir == fcbId)
            {
                showTreeRecursive(i, depth + 1, userId);
            }
        }
    }
    else
    {
        cout << "├──  " << fcb.name
             << " (size: " << fcb.size
             << ", mtime: " << formatTime(fcb.modifyTime) << ")" << endl;
    }
}

void MiniFMS::userInteractionThread(Session &session)
{
    string inputBuffer;
    getline(cin, inputBuffer);

    while (session.active && !shouldExit)
    {
        cout << "\033[1;32m" << session.user->username << "@MiniFMS\033[0m:"
             << "\033[1;34m" << getCurrentPath(session.currentDirId, session.user->userId) << "\033[0m$ ";

        string cmdline;
        getline(cin, cmdline);

        if (cmdline == "exit" || shouldExit)
        {
            // 用户退出时保存数据
            if (dataChanged)
            {
                cout << "正在保存会话数据..." << endl;
                if (saveDataToDisk(true))
                {
                    cout << "会话数据已保存" << endl;
                }
                else
                {
                    cout << "警告：数据保存失败！" << endl;
                }
            }
            session.active = false;
            cout << "Bye! 感谢使用 MiniFMS." << endl;
            shouldExit = true;
            break;
        }

        if (cmdline.empty())
            continue;

        {
            lock_guard<mutex> lock(queueMutex);
            commandQueue.push(CommandRequest(&session, cmdline));
        }
        queueCv.notify_one();

        {
            unique_lock<mutex> lock(queueMutex);
            queueCv.wait(lock, [this]
                         { return ready || shouldExit; });
            if (shouldExit)
            {
                break;
            }
            ready = false;
        }
    }
}

void MiniFMS::diskMaintenanceThread()
{
    while (!shouldExit)
    {
        CommandRequest req;
        {
            unique_lock<mutex> lock(queueMutex);
            queueCv.wait(lock, [this]
                         { return !commandQueue.empty() || shouldExit; });

            if (shouldExit)
            {
                break;
            }

            req = commandQueue.front();
            commandQueue.pop();
        }

        processCommand(req);

        {
            lock_guard<mutex> lock(queueMutex);
            ready = true;
        }
        queueCv.notify_one();
    }
}

void MiniFMS::processCommand(CommandRequest &req)
{
    istringstream iss(req.commandLine);
    string cmd;
    vector<string> args;
    iss >> cmd;
    string arg;
    while (iss >> arg)
        args.push_back(arg); // 将命令行参数分割成多个字符串
    if (cmd == "help")
    {
        showHelp();
    }
    else if (cmd == "dir")
    {
        listDirectory(req.session);
    }
    else if (cmd == "mkdir")
    {
        if (args.empty())
        {
            cout << " 用法: mkdir [目录名]" << endl;
        }
        else
        {
            if (findFCB(req.session->currentDirId, args[0]) != -1)
            {
                cout << " 目录已存在: " << args[0] << endl;
            }
            else
            {
                int newDirId = createFCB(args[0], 1, req.session->user->userId, req.session->currentDirId);
                if (newDirId != -1)
                {
                    cout << " 目录创建成功: " << args[0] << endl;
                }
                else
                {
                    cout << " 目录创建失败" << endl;
                }
            }
        }
    }
    else if (cmd == "rmdir")
    {
        if (args.empty())
        {
            cout << " 用法: rmdir [目录名]" << endl;
            cout << " 说明: 删除指定的目录" << endl;
            cout << " 选项: -f  强制删除非空目录" << endl;
            return;
        }

        bool forceDelete = false;
        string dirName = args[0];

        // 检查是否有 -f 选项
        if (args.size() > 1 && args[1] == "-f")
        {
            forceDelete = true;
        }

        int dirId = findFCB(req.session->currentDirId, dirName);
        if (dirId == -1)
        {
            cout << " 错误: 目录不存在: " << dirName << endl;
            return;
        }

        if (sharedData->fcbs[dirId].type != 1)
        {
            cout << " 错误: " << dirName << " 不是一个目录" << endl;
            return;
        }

        // 检查是否有权限删除
        if (sharedData->fcbs[dirId].owner != req.session->user->userId)
        {
            cout << " 错误: 权限不足，无法删除其他用户的目录" << endl;
            return;
        }

        // 检查目录是否为空
        bool isEmpty = true;
        int fileCount = 0;
        vector<pair<int, string>> contents; // <fcbId, name>

        for (int i = 0; i < MAX_FCBS; i++)
        {
            if (sharedData->fcbs[i].isused && sharedData->fcbs[i].parentDir == dirId)
            {
                isEmpty = false;
                fileCount++;
                contents.push_back({i, string(sharedData->fcbs[i].name)});
            }
        }

        if (!isEmpty)
        {
            if (!forceDelete)
            {
                cout << " 错误: 目录不为空，使用 rmdir " << dirName << " -f 强制删除" << endl;
                return;
            }

            cout << " 警告: 目录 " << dirName << " 不为空" << endl;
            cout << " 包含 " << fileCount << " 个文件/子目录:" << endl;
            for (const auto &item : contents)
            {
                string itemType = sharedData->fcbs[item.first].type == 1 ? "目录" : "文件";
                cout << "   - " << itemType << ": " << item.second << endl;
            }

            cout << "\n 确认要删除此目录及其所有内容吗? (y/n): ";
            string confirm;
            if (!getline(cin, confirm))
            {
                cout << " 输入错误，操作已取消" << endl;
                return;
            }

            if (confirm != "y" && confirm != "Y")
            {
                cout << " 操作已取消" << endl;
                return;
            }

            cout << "\n 正在删除目录 " << dirName << " 及其内容..." << endl;

            // 删除所有内容
            for (const auto &item : contents)
            {
                int fcbId = item.first;
                string itemType = sharedData->fcbs[fcbId].type == 1 ? "目录" : "文件";
                cout << " - 删除" << itemType << ": " << item.second << endl;

                // 清理FCB
                sharedData->fcbs[fcbId].isused = 0;
                memset(sharedData->fcbs[fcbId].name, 0, MAX_FILENAME_LEN);
                sharedData->fcbs[fcbId].type = 0;
                sharedData->fcbs[fcbId].size = 0;
                sharedData->fcbs[fcbId].parentDir = -1;
                sharedData->fcbs[fcbId].owner = -1;

                // 如果是文件，清空内容
                if (sharedData->fcbs[fcbId].type == 0)
                {
                    memset(sharedData->fileContents[fcbId], 0, sizeof(sharedData->fileContents[fcbId]));
                }
            }
        }

        // 删除目录本身
        sharedData->fcbs[dirId].isused = 0;
        memset(sharedData->fcbs[dirId].name, 0, MAX_FILENAME_LEN);
        sharedData->fcbs[dirId].type = 0;
        sharedData->fcbs[dirId].size = 0;
        sharedData->fcbs[dirId].parentDir = -1;
        sharedData->fcbs[dirId].owner = -1;

        cout << " 目录删除成功: " << dirName << endl;

        // 保存更改
        sharedData->modifyCount++;
        dataChanged = true;
        saveDataToDisk(true);
    }
    else if (cmd == "tree")
    {
        showTree(req.session);
    }
    else if (cmd == "save")
    {
        cout << " 正在保存数据到磁盘..." << endl;
        if (saveDataToDisk(false)) // 显式使用非静默模式
        {
            cout << " 数据保存成功!" << endl;
        }
        else
        {
            cout << " 数据保存失败!" << endl;
        }
    }
    else if (cmd == "create")
    {
        if (args.empty())
        {
            cout << " 用法: create [文件名]" << endl;
        }
        else
        {
            createFile(req.session, args[0]);
        }
    }
    else if (cmd == "delete")
    {
        if (args.empty())
        {
            cout << " 用法: delete [文件名]" << endl;
        }
        else
        {
            deleteFile(req.session, args[0]);
        }
    }
    else if (cmd == "open")
    {
        if (args.size() < 2)
        {
            cout << " 用法: open [文件名] [模式] (r/w/rw)" << endl;
            cout << " 示例: open test.txt r  # 以只读模式打开文件" << endl;
            cout << " 注意: 同一文件不能重复打开，需要先close后才能重新open" << endl;
        }
        else
        {
            int fileId = findFCB(req.session->currentDirId, args[0]);
            if (fileId == -1 || sharedData->fcbs[fileId].type != 0)
            {
                cout << " 文件不存在: " << args[0] << endl;
            }
            else
            {
                int mode = 0;
                if (args[1] == "w")
                    mode = 1;
                else if (args[1] == "rw")
                    mode = 2;
                else if (args[1] != "r")
                {
                    cout << " 无效的打开模式，请使用 r/w/rw" << endl;
                    return;
                }

                // 检查文件是否已经被打开
                bool alreadyOpen = false;
                int existingFd = -1;
                for (size_t i = 0; i < req.session->openFiles.size(); ++i)
                {
                    if (req.session->openFiles[i].isOpen &&
                        req.session->openFiles[i].fcbId == fileId)
                    {
                        alreadyOpen = true;
                        existingFd = static_cast<int>(i);
                        break;
                    }
                }

                if (alreadyOpen)
                {
                    string currentMode;
                    switch (req.session->openFiles[existingFd].mode)
                    {
                    case 0:
                        currentMode = "只读";
                        break;
                    case 1:
                        currentMode = "只写";
                        break;
                    case 2:
                        currentMode = "读写";
                        break;
                    }
                    cout << " 错误：文件 " << args[0] << " 已经被打开" << endl;
                    cout << " 当前打开状态：文件描述符 = " << existingFd << ", 模式 = " << currentMode << endl;
                    cout << " 提示：如需以其他模式打开，请先使用 close " << existingFd << " 关闭文件" << endl;
                }
                else
                {
                    int fd = req.session->addOpenFile(fileId, mode);
                    cout << " 文件打开成功，文件描述符: " << fd << endl;
                }
            }
        }
    }
    else if (cmd == "close")
    {
        if (args.empty())
        {
            cout << " 用法: close [文件描述符]" << endl;
        }
        else
        {
            try
            {
                int fd = stoi(args[0]);
                if (req.session->closeFile(fd))
                {
                    cout << " 文件已关闭" << endl;
                }
                else
                {
                    cout << " 无效的文件描述符" << endl;
                }
            }
            catch (const std::invalid_argument &e)
            {
                cout << " 文件描述符必须是数字: " << args[0] << endl;
            }
            catch (const std::out_of_range &e)
            {
                cout << " 文件描述符超出范围: " << args[0] << endl;
            }
        }
    }
    else if (cmd == "read")
    {
        if (args.empty())
        {
            cout << " 用法: read [文件描述符] [可选:要读取的字节数]" << endl;
            cout << " 示例: read 0     # 从当前位置读取到文件末尾" << endl;
            cout << "       read 0 10  # 从当前位置读取10个字节" << endl;
        }
        else
        {
            try
            {
                int fd = stoi(args[0]);
                if (fd >= 0 && fd < static_cast<int>(req.session->openFiles.size()) &&
                    req.session->openFiles[fd].isOpen)
                {
                    FileDesc &fileDesc = req.session->openFiles[fd];
                    if (fileDesc.mode == 1)
                    {
                        cout << " 文件以只写模式打开" << endl;
                    }
                    else
                    {
                        int fcbId = fileDesc.fcbId;
                        string content = string(sharedData->fileContents[fcbId]);

                        // 如果指定了读取长度
                        if (args.size() > 1)
                        {
                            size_t length = stoi(args[1]);
                            if (fileDesc.position + length > content.length())
                            {
                                cout << " 警告：请求读取的长度超出文件末尾，将只读取到文件末尾" << endl;
                                length = content.length() - fileDesc.position;
                            }
                            cout << " 从位置 " << fileDesc.position << " 读取 " << length << " 个字节:" << endl;
                            cout << content.substr(fileDesc.position, length) << endl;
                            fileDesc.position += length;
                        }
                        else
                        {
                            // 读取从当前位置到文件末尾的所有内容
                            if (fileDesc.position >= content.length())
                            {
                                cout << " 已到达文件末尾" << endl;
                            }
                            else
                            {
                                cout << " 从位置 " << fileDesc.position << " 读取到文件末尾:" << endl;
                                cout << content.substr(fileDesc.position) << endl;
                                fileDesc.position = content.length();
                            }
                        }

                        sharedData->fcbs[fcbId].accessTime = time(nullptr);
                        cout << " 当前文件指针位置：" << fileDesc.position << endl;
                    }
                }
                else
                {
                    cout << " 无效的文件描述符" << endl;
                }
            }
            catch (const std::invalid_argument &e)
            {
                cout << " 文件描述符必须是数字: " << args[0] << endl;
            }
            catch (const std::out_of_range &e)
            {
                cout << " 文件描述符超出范围: " << args[0] << endl;
            }
        }
    }
    else if (cmd == "write")
    {
        if (args.empty())
        {
            cout << " 用法: write [文件描述符] [-a/-o]" << endl;
            cout << " 选项: -a 从当前位置追加内容" << endl;
            cout << "       -o 覆盖当前位置的内容" << endl;
            cout << " 示例: write 0 -a  # 在当前位置追加内容" << endl;
            cout << "       write 0 -o  # 覆盖当前位置的内容" << endl;
        }
        else
        {
            try
            {
                int fd = stoi(args[0]);
                if (fd >= 0 && fd < static_cast<int>(req.session->openFiles.size()) &&
                    req.session->openFiles[fd].isOpen)
                {
                    FileDesc &fileDesc = req.session->openFiles[fd];
                    if (fileDesc.mode == 0)
                    {
                        cout << " 文件以只读模式打开" << endl;
                        return;
                    }

                    // 检查文件访问权限
                    if (!checkFileAccess(req.session, fileDesc.fcbId, true))
                    {
                        return;
                    }

                    bool isOverwrite = args.size() > 1 && args[1] == "-o";

                    cout << " 请输入内容 (以EOF或单独的.结束): " << endl;
                    string content, line;
                    while (getline(cin, line) && line != ".")
                    {
                        content += line + "\n";
                    }

                    int fcbId = fileDesc.fcbId;
                    string fileContent = string(sharedData->fileContents[fcbId]);

                    // 根据写入模式处理内容
                    if (isOverwrite)
                    {
                        // 如果是覆盖模式，检查是否会超出文件大小限制
                        size_t newSize;
                        if (fileDesc.position + content.length() > fileContent.length())
                        {
                            newSize = fileDesc.position + content.length();
                        }
                        else
                        {
                            newSize = fileContent.length();
                        }

                        if (newSize >= sizeof(sharedData->fileContents[fcbId]))
                        {
                            cout << " 错误：写入后文件大小超出限制" << endl;
                            return;
                        }

                        // 如果需要，扩展文件大小
                        if (fileDesc.position > fileContent.length())
                        {
                            fileContent.append(fileDesc.position - fileContent.length(), '\0');
                        }

                        // 覆盖内容
                        fileContent.replace(fileDesc.position, content.length(), content);
                    }
                    else
                    {
                        // 追加模式
                        if (fileDesc.position + content.length() >= sizeof(sharedData->fileContents[fcbId]))
                        {
                            cout << " 错误：写入后文件大小超出限制" << endl;
                            return;
                        }

                        // 如果文件指针不在文件末尾，先移动到文件末尾
                        if (fileDesc.position < fileContent.length())
                        {
                            fileContent.insert(fileDesc.position, content);
                        }
                        else
                        {
                            // 如果文件指针超出文件末尾，先填充空字符
                            if (fileDesc.position > fileContent.length())
                            {
                                fileContent.append(fileDesc.position - fileContent.length(), '\0');
                            }
                            fileContent.append(content);
                        }
                    }

                    // 更新文件内容
                    strncpy(sharedData->fileContents[fcbId], fileContent.c_str(),
                            sizeof(sharedData->fileContents[fcbId]) - 1);
                    sharedData->fcbs[fcbId].size = fileContent.length();
                    sharedData->fcbs[fcbId].modifyTime = time(nullptr);

                    // 更新文件指针位置
                    fileDesc.position += content.length();

                    cout << " 写入成功" << endl;
                    cout << " - 写入字节数：" << content.length() << endl;
                    cout << " - 当前文件指针位置：" << fileDesc.position << endl;
                    cout << " - 当前文件大小：" << sharedData->fcbs[fcbId].size << endl;

                    sharedData->modifyCount++;
                    dataChanged = true;
                }
                else
                {
                    cout << " 无效的文件描述符" << endl;
                }
            }
            catch (const exception &e)
            {
                cout << " 参数错误: " << e.what() << endl;
            }
        }
    }
    else if (cmd == "copy")
    {
        if (args.size() < 2)
        {
            cout << " 用法: copy [源文件名] [目标目录路径]" << endl;
            cout << " 支持的路径格式：" << endl;
            cout << "   - 相对路径: docs/backup/     # 当前目录下的子目录" << endl;
            cout << "   - 上级目录: ../backup/       # 返回上级目录" << endl;
            cout << "   - 绝对路径: /docs/backup/    # 从根目录开始" << endl;
            cout << "   - 当前目录: ./backup/        # 当前目录" << endl;
        }
        else
        {
            // 检查源文件是否存在
            int srcId = findFCB(req.session->currentDirId, args[0]);
            if (srcId == -1 || sharedData->fcbs[srcId].type != 0)
            {
                cout << " 源文件不存在: " << args[0] << endl;
                return;
            }

            // 解析目标路径
            string targetPath = args[1];
            if (targetPath.empty())
            {
                cout << " 错误：目标路径不能为空" << endl;
                return;
            }

            // 确保路径以/结尾
            if (targetPath[targetPath.length() - 1] != '/')
            {
                targetPath += '/';
            }

            // 移除末尾的/并查找目标目录
            string pathForSearch = targetPath.substr(0, targetPath.length() - 1);
            int targetDirId = findFCBByPath(req.session, pathForSearch);
            if (targetDirId == -1)
            {
                cout << " 目标目录不存在: " << pathForSearch << endl;
                return;
            }

            // 确认是目录
            if (sharedData->fcbs[targetDirId].type != 1)
            {
                cout << " 错误：" << pathForSearch << " 不是一个目录" << endl;
                return;
            }

            // 检查目标目录中是否已存在同名文件
            if (findFCB(targetDirId, args[0]) != -1)
            {
                cout << " 目标目录中已存在同名文件: " << args[0] << endl;
                return;
            }

            // 在目标目录中创建新文件
            int newFileId = createFCB(args[0], 0, req.session->user->userId, targetDirId);
            if (newFileId != -1)
            {
                // 复制文件内容
                memcpy(sharedData->fileContents[newFileId],
                       sharedData->fileContents[srcId],
                       sizeof(sharedData->fileContents[srcId]));
                sharedData->fcbs[newFileId].size = sharedData->fcbs[srcId].size;
                sharedData->fcbs[newFileId].modifyTime = time(nullptr);

                cout << " 文件复制成功: " << endl;
                cout << " - 源文件: " << args[0] << endl;
                cout << " - 目标位置: " << pathForSearch << "/" << args[0] << endl;
                cout << " - 文件大小: " << sharedData->fcbs[newFileId].size << " 字节" << endl;

                sharedData->modifyCount++;
                dataChanged = true;
            }
            else
            {
                cout << " 文件复制失败" << endl;
            }
        }
    }
    else if (cmd == "move")
    {
        if (args.size() < 2)
        {
            cout << " 用法: move [源文件名] [目标目录路径]" << endl;
            cout << " 支持的路径格式：" << endl;
            cout << "   - 相对路径: docs/backup/     # 当前目录下的子目录" << endl;
            cout << "   - 上级目录: ../backup/       # 返回上级目录" << endl;
            cout << "   - 绝对路径: /docs/backup/    # 从根目录开始" << endl;
            cout << "   - 当前目录: ./backup/        # 当前目录" << endl;
        }
        else
        {
            // 检查源文件是否存在
            int srcId = findFCB(req.session->currentDirId, args[0]);
            if (srcId == -1 || sharedData->fcbs[srcId].type != 0)
            {
                cout << " 源文件不存在: " << args[0] << endl;
                return;
            }

            // 解析目标路径
            string targetPath = args[1];
            if (targetPath.empty())
            {
                cout << " 错误：目标路径不能为空" << endl;
                return;
            }

            // 确保路径以/结尾
            if (targetPath[targetPath.length() - 1] != '/')
            {
                targetPath += '/';
            }

            // 移除末尾的/并查找目标目录
            string pathForSearch = targetPath.substr(0, targetPath.length() - 1);
            int targetDirId = findFCBByPath(req.session, pathForSearch);
            if (targetDirId == -1)
            {
                cout << " 目标目录不存在: " << pathForSearch << endl;
                return;
            }

            // 确认是目录
            if (sharedData->fcbs[targetDirId].type != 1)
            {
                cout << " 错误：" << pathForSearch << " 不是一个目录" << endl;
                return;
            }

            // 检查目标目录中是否已存在同名文件
            if (findFCB(targetDirId, args[0]) != -1)
            {
                cout << " 目标目录中已存在同名文件: " << args[0] << endl;
                return;
            }

            // 移动文件（更新父目录）
            sharedData->fcbs[srcId].parentDir = targetDirId;
            sharedData->fcbs[srcId].modifyTime = time(nullptr);

            cout << " 文件移动成功: " << endl;
            cout << " - 源文件: " << args[0] << endl;
            cout << " - 目标位置: " << pathForSearch << "/" << args[0] << endl;

            sharedData->modifyCount++;
            dataChanged = true;
        }
    }
    else if (cmd == "flock")
    {
        if (args.empty())
        {
            cout << " 用法: flock [文件名]" << endl;
            cout << " 功能: 锁定/解锁文件，将文件设置为只读状态" << endl;
            cout << " 说明: - 锁定的文件所有用户（包括锁定者）都只能读取" << endl;
            cout << "       - 任何用户都不能修改锁定的文件" << endl;
            cout << "       - 只有锁定者可以解锁文件" << endl;
            cout << "       - 使用相同命令可以解锁文件" << endl;
        }
        else
        {
            int fileId = findFCB(req.session->currentDirId, args[0]);
            if (fileId == -1 || sharedData->fcbs[fileId].type != 0)
            {
                cout << " 文件不存在: " << args[0] << endl;
            }
            else
            {
                FCB &fcb = sharedData->fcbs[fileId];

                // 检查文件是否已经被打开
                bool isFileOpen = false;
                for (const auto &openFile : req.session->openFiles)
                {
                    if (openFile.isOpen && openFile.fcbId == fileId)
                    {
                        isFileOpen = true;
                        break;
                    }
                }

                if (fcb.locked)
                {
                    if (fcb.lockOwner == req.session->user->userId)
                    {
                        fcb.locked = false;
                        fcb.lockOwner = -1;
                        cout << " 文件解锁成功: " << args[0] << endl;

                        // 显示文件状态
                        cout << " 当前状态：" << endl;
                        cout << " - 锁定状态：未锁定" << endl;
                        cout << " - 文件现在可以读写" << endl;
                    }
                    else
                    {
                        cout << " 错误：文件当前被其他用户锁定" << endl;
                        // 显示锁定信息
                        for (int i = 0; i < MAX_USERS; i++)
                        {
                            if (sharedData->users[i].isused &&
                                sharedData->users[i].userId == fcb.lockOwner)
                            {
                                cout << " - 锁定者：" << sharedData->users[i].username << endl;
                                break;
                            }
                        }
                        cout << " - 锁定时间：" << formatTime(fcb.modifyTime) << endl;
                        cout << " - 文件处于只读状态" << endl;
                    }
                }
                else
                {
                    if (isFileOpen)
                    {
                        cout << " 警告：文件已打开，建议先关闭文件再加锁" << endl;
                    }

                    fcb.locked = true;
                    fcb.lockOwner = req.session->user->userId;
                    fcb.modifyTime = time(nullptr);
                    cout << " 文件加锁成功: " << args[0] << endl;

                    // 显示文件状态
                    cout << " 当前状态：" << endl;
                    cout << " - 锁定状态：已锁定（只读）" << endl;
                    cout << " - 锁定者：" << req.session->user->username << endl;
                    cout << " - 锁定时间：" << formatTime(fcb.modifyTime) << endl;
                    cout << " - 所有用户（包括锁定者）只能读取此文件" << endl;
                }

                // 标记数据已修改
                sharedData->modifyCount++;
                dataChanged = true;
            }
        }
    }
    else if (cmd == "head")
    {
        if (args.size() < 2)
        {
            cout << " 用法: head -num [文件名]" << endl;
            cout << " 示例: head -5 test.txt  # 显示文件前5行" << endl;
        }
        else
        {
            try
            {
                string numStr = args[0];
                if (numStr[0] != '-')
                {
                    cout << " 参数格式错误，应为 -num" << endl;
                    return;
                }
                int numLines = stoi(numStr.substr(1));
                if (numLines <= 0)
                {
                    cout << " 行数必须大于0" << endl;
                    return;
                }
                showFileHead(req.session, args[1], numLines);
            }
            catch (const exception &e)
            {
                cout << " 参数错误: " << e.what() << endl;
            }
        }
    }
    else if (cmd == "tail")
    {
        if (args.size() < 2)
        {
            cout << " 用法: tail -num [文件名]" << endl;
            cout << " 示例: tail -5 test.txt  # 显示文件后5行" << endl;
        }
        else
        {
            try
            {
                string numStr = args[0];
                if (numStr[0] != '-')
                {
                    cout << " 参数格式错误，应为 -num" << endl;
                    return;
                }
                int numLines = stoi(numStr.substr(1));
                if (numLines <= 0)
                {
                    cout << " 行数必须大于0" << endl;
                    return;
                }
                showFileTail(req.session, args[1], numLines);
            }
            catch (const exception &e)
            {
                cout << " 参数错误: " << e.what() << endl;
            }
        }
    }
    else if (cmd == "lseek")
    {
        if (args.size() < 2)
        {
            cout << " 用法: lseek [文件描述符] [偏移量]" << endl;
            cout << " 示例: lseek 0 10  # 从当前位置向后移动10个字节" << endl;
            cout << "       lseek 0 -5  # 从当前位置向前移动5个字节" << endl;
        }
        else
        {
            try
            {
                int fd = stoi(args[0]);
                int offset = stoi(args[1]);

                if (fd >= 0 && fd < static_cast<int>(req.session->openFiles.size()) &&
                    req.session->openFiles[fd].isOpen)
                {
                    FileDesc &fileDesc = req.session->openFiles[fd];
                    int fcbId = fileDesc.fcbId;

                    // 检查文件访问权限
                    if (!checkFileAccess(req.session, fcbId, true))
                    {
                        return;
                    }

                    // 计算新位置
                    size_t newPosition = fileDesc.position + offset;
                    size_t fileSize = sharedData->fcbs[fcbId].size;

                    // 检查新位置是否有效
                    if (newPosition > fileSize)
                    {
                        cout << " 错误：移动位置超出文件范围" << endl;
                        cout << " - 当前位置：" << fileDesc.position << endl;
                        cout << " - 文件大小：" << fileSize << endl;
                        cout << " - 请求偏移：" << offset << endl;
                        return;
                    }

                    // 更新文件指针位置
                    fileDesc.position = newPosition;
                    cout << " 文件指针已移动到：" << newPosition << endl;

                    // 如果需要写入内容
                    cout << " 是否要在当前位置写入内容？(y/n): ";
                    string choice;
                    getline(cin, choice);

                    if (choice == "y" || choice == "Y")
                    {
                        if (fileDesc.mode == 0)
                        {
                            cout << " 错误：文件以只读模式打开" << endl;
                            return;
                        }

                        cout << " 请输入要写入的内容：";
                        string content;
                        getline(cin, content);

                        // 获取原文件内容
                        string fileContent = string(sharedData->fileContents[fcbId]);

                        // 在指定位置插入新内容
                        if (newPosition == fileSize)
                        {
                            fileContent += content;
                        }
                        else
                        {
                            fileContent.insert(newPosition, content);
                        }

                        // 检查文件大小限制
                        if (fileContent.length() >= sizeof(sharedData->fileContents[fcbId]))
                        {
                            cout << " 错误：写入后文件大小超出限制" << endl;
                            return;
                        }

                        // 更新文件内容
                        strncpy(sharedData->fileContents[fcbId], fileContent.c_str(),
                                sizeof(sharedData->fileContents[fcbId]) - 1);
                        sharedData->fcbs[fcbId].size = fileContent.length();
                        sharedData->fcbs[fcbId].modifyTime = time(nullptr);

                        // 更新文件指针位置
                        fileDesc.position += content.length();

                        cout << " 内容写入成功" << endl;
                        cout << " - 当前文件指针位置：" << fileDesc.position << endl;
                        cout << " - 当前文件大小：" << sharedData->fcbs[fcbId].size << endl;

                        sharedData->modifyCount++;
                        dataChanged = true;
                    }
                }
                else
                {
                    cout << " 无效的文件描述符" << endl;
                }
            }
            catch (const std::invalid_argument &e)
            {
                cout << " 参数必须是数字" << endl;
            }
            catch (const std::out_of_range &e)
            {
                cout << " 参数超出范围" << endl;
            }
        }
    }
    else if (cmd == "cd")
    {
        if (args.empty())
        {
            cout << " 用法: cd [目录名]" << endl;
        }
        else
        {
            if (args[0] == "..")
            {
                if (req.session->currentDirId != req.session->user->rootDirId)
                {
                    int parentId = sharedData->fcbs[req.session->currentDirId].parentDir;
                    if (parentId >= 0)
                    {
                        req.session->currentDirId = parentId;
                        cout << " 已切换到上级目录" << endl;
                    }
                }
                else
                {
                    cout << " 已在根目录" << endl;
                }
            }
            else
            {
                int targetDir = findFCB(req.session->currentDirId, args[0]);
                if (targetDir != -1 && sharedData->fcbs[targetDir].type == 1)
                {
                    req.session->currentDirId = targetDir;
                    sharedData->fcbs[targetDir].accessTime = time(nullptr);
                    cout << " 已切换到目录: " << args[0] << endl;
                }
                else
                {
                    cout << " 目录不存在: " << args[0] << endl;
                }
            }
        }
    }
    else if (cmd.empty())
    {
        // 空命令不处理
    }
    else if (cmd == "import")
    {
        if (args.empty())
        {
            cout << " 用法: import [外部文件路径] [系统内文件名]" << endl;
            cout << " 说明: 将外部文件导入到文件系统中" << endl;
            return;
        }

        string externalPath = args[0];
        string internalName = args.size() > 1 ? args[1] : externalPath.substr(externalPath.find_last_of("/\\") + 1);
        importFile(req.session, externalPath, internalName);
    }
    else if (cmd == "export")
    {
        if (args.empty())
        {
            cout << " 用法: export [系统内文件名] [外部文件路径]" << endl;
            cout << " 说明: 将文件系统中的文件导出到外部" << endl;
            return;
        }

        string internalName = args[0];
        string externalPath = args.size() > 1 ? args[1] : internalName;
        exportFile(req.session, internalName, externalPath);
    }
    else
    {
        cout << " " << cmd << ": command not found" << endl;
        cout << " 输入 'help' 查看可用命令" << endl;
    }
}

void MiniFMS::run()
{
    cout << "\n正在启动 MiniFMS Pro...\n"
         << endl;

    while (!shouldExit)
    {
        cout << "\n╔════════════════════════════════╗" << endl;
        cout << "║    MiniFMS " << VERSION << "                ║" << endl;
        cout << "║        文件管理系统              ║" << endl;
        cout << "╠════════════════════════════════╣" << endl;
        cout << "║  1.  用户注册                  ║" << endl;
        cout << "║  2.  用户登录                  ║" << endl;
        cout << "║  0.  退出系统                  ║" << endl;
        cout << "╚════════════════════════════════╝" << endl;
        cout << "\n请输入选项 (0-2): ";

        string input;
        if (!getline(cin, input))
        {
            cout << "\n输入错误，正在保存数据..." << endl;
            shouldExit = true;
            break;
        }

        int choice;
        try
        {
            choice = stoi(input);
        }
        catch (...)
        {
            cout << "\n无效的输入，请输入0-2之间的数字" << endl;
            continue;
        }

        if (choice == 1)
        {
            cout << "\n=== 用户注册 ===" << endl;
            cout << "请输入用户名: ";
            string username;
            if (!getline(cin, username) || username.empty())
            {
                cout << "用户名不能为空！" << endl;
                continue;
            }

            cout << "请输入密码: ";
            string password;
            if (!getline(cin, password) || password.empty())
            {
                cout << "密码不能为空！" << endl;
                continue;
            }

            registerUser(username, password);
        }
        else if (choice == 2)
        {
            cout << "\n=== 用户登录 ===" << endl;
            cout << "用户名: ";
            string username;
            if (!getline(cin, username) || username.empty())
            {
                cout << "用户名不能为空！" << endl;
                continue;
            }

            cout << "密码: ";
            string password;
            if (!getline(cin, password) || password.empty())
            {
                cout << "密码不能为空！" << endl;
                continue;
            }

            User *user = loginUser(username, password);
            if (user)
            {
                currentSession.user = user;
                currentSession.active = true;
                currentSession.currentDirId = user->rootDirId;

                // 启动自动保存线程
                autoSaveThreadHandle = thread(&MiniFMS::autoSaveThread, this);

                // 启动磁盘维护线程
                diskMaintenanceThreadHandle = thread(&MiniFMS::diskMaintenanceThread, this);

                // 进入用户交互
                userInteractionThread(currentSession);

                // 等待线程结束
                cleanup();

                // 重置用户状态
                user->isActive = false;
            }
        }
        else if (choice == 0)
        {
            cout << "\n正在保存系统数据..." << endl;
            shouldExit = true;
            if (saveDataToDisk(false))
            {
                cout << "系统数据已保存" << endl;
            }
            else
            {
                cout << "警告：系统数据保存失败！" << endl;
            }
            cout << "感谢使用 MiniFMS，再见！" << endl;
            break;
        }
        else
        {
            cout << "\n无效的选项，请重新选择！" << endl;
        }
    }

    // 确保所有资源都被清理
    cleanup();
}

// 持久化功能实现
bool MiniFMS::saveDataToDisk(bool silent)
{
    if (!sharedData)
        return false;

    lock_guard<mutex> lock(diskMutex);

    try
    {
        // 以二进制方式打开文件
        ofstream file("filesystem.dat", ios::binary | ios::trunc);
        if (!file.is_open())
        {
            if (!silent)
                cerr << " 无法创建数据文件" << endl;
            return false;
        }

        // 1. 写入文件头部标识和版本信息
        const char MAGIC[] = "MINIFMS2";
        file.write(MAGIC, 8);
        int version = 1;
        file.write(reinterpret_cast<const char *>(&version), sizeof(version));

        // 2. 写入用户数据
        int userCount = 0;
        for (int i = 0; i < MAX_USERS; i++)
        {
            if (sharedData->users[i].isused)
                userCount++;
        }
        file.write(reinterpret_cast<const char *>(&userCount), sizeof(userCount));

        for (int i = 0; i < MAX_USERS; i++)
        {
            if (sharedData->users[i].isused)
            {
                file.write(reinterpret_cast<const char *>(&sharedData->users[i]), sizeof(User));
            }
        }

        // 3. 写入文件系统数据
        int fcbCount = 0;
        for (int i = 0; i < MAX_FCBS; i++)
        {
            if (sharedData->fcbs[i].isused)
                fcbCount++;
        }
        file.write(reinterpret_cast<const char *>(&fcbCount), sizeof(fcbCount));

        // 写入FCB和文件内容
        for (int i = 0; i < MAX_FCBS; i++)
        {
            if (sharedData->fcbs[i].isused)
            {
                // 写入FCB
                file.write(reinterpret_cast<const char *>(&sharedData->fcbs[i]), sizeof(FCB));
                // 如果是文件类型，写入文件内容
                if (sharedData->fcbs[i].type == 0)
                {
                    file.write(sharedData->fileContents[i], sizeof(sharedData->fileContents[i]));
                }
            }
        }

        // 4. 写入系统状态
        file.write(reinterpret_cast<const char *>(&sharedData->modifyCount), sizeof(sharedData->modifyCount));
        file.write(reinterpret_cast<const char *>(&sharedData->nextUserId), sizeof(sharedData->nextUserId));
        file.write(reinterpret_cast<const char *>(&sharedData->nextFcbId), sizeof(sharedData->nextFcbId));

        file.flush();
        file.close();
        dataChanged = false;

        if (!silent)
        {
            cout << " 数据已保存到文件 filesystem.dat" << endl;
            cout << " 已保存 " << userCount << " 个用户, " << fcbCount << " 个文件/目录" << endl;
        }

        return true;
    }
    catch (const exception &e)
    {
        if (!silent)
            cerr << " 保存数据失败: " << e.what() << endl;
        return false;
    }
}

bool MiniFMS::loadDataFromDisk()
{
    if (!sharedData)
        return false;

    try
    {
        ifstream file("filesystem.dat", ios::binary);
        if (!file.is_open())
        {
            cout << " 数据文件不存在，将创建新的文件系统" << endl;
            return false;
        }

        // 1. 验证文件头部标识和版本
        char magic[9] = {0};
        file.read(magic, 8);
        if (strcmp(magic, "MINIFMS2") != 0)
        {
            cerr << " 数据文件格式错误" << endl;
            return false;
        }

        int version;
        file.read(reinterpret_cast<char *>(&version), sizeof(version));
        if (version != 1)
        {
            cerr << " 数据文件版本不兼容" << endl;
            return false;
        }

        // 2. 读取用户数据
        int userCount;
        file.read(reinterpret_cast<char *>(&userCount), sizeof(userCount));

        // 清空现有数据 - 使用默认构造函数初始化
        for (int i = 0; i < MAX_USERS; i++)
        {
            sharedData->users[i] = User();
        }

        for (int i = 0; i < userCount; i++)
        {
            User user;
            file.read(reinterpret_cast<char *>(&user), sizeof(User));
            sharedData->users[i] = user;
        }

        // 3. 读取文件系统数据
        int fcbCount;
        file.read(reinterpret_cast<char *>(&fcbCount), sizeof(fcbCount));

        // 清空现有数据 - 使用默认构造函数初始化
        for (int i = 0; i < MAX_FCBS; i++)
        {
            sharedData->fcbs[i] = FCB();
        }
        memset(sharedData->fileContents, 0, sizeof(sharedData->fileContents)); // 这个是普通数组，可以用memset

        for (int i = 0; i < fcbCount; i++)
        {
            FCB fcb;
            file.read(reinterpret_cast<char *>(&fcb), sizeof(FCB));
            int fcbIndex = fcb.address;
            sharedData->fcbs[fcbIndex] = fcb;

            // 如果是文件类型，读取文件内容
            if (fcb.type == 0)
            {
                file.read(sharedData->fileContents[fcbIndex], sizeof(sharedData->fileContents[fcbIndex]));
            }
        }

        // 4. 读取系统状态
        file.read(reinterpret_cast<char *>(&sharedData->modifyCount), sizeof(sharedData->modifyCount));
        file.read(reinterpret_cast<char *>(&sharedData->nextUserId), sizeof(sharedData->nextUserId));
        file.read(reinterpret_cast<char *>(&sharedData->nextFcbId), sizeof(sharedData->nextFcbId));

        file.close();
        sharedData->initialized = true;

        cout << " 从文件 filesystem.dat 加载数据成功" << endl;
        cout << " 已加载 " << userCount << " 个用户, " << fcbCount << " 个文件/目录" << endl;

        return true;
    }
    catch (const exception &e)
    {
        cerr << " 加载数据失败: " << e.what() << endl;
        return false;
    }
}

void MiniFMS::autoSaveThread()
{
    while (!shouldExit)
    {
        this_thread::sleep_for(chrono::seconds(30)); // 每30秒检查一次

        if (shouldExit)
        {
            break;
        }

        if (dataChanged && sharedData && sharedData->initialized)
        {
            // 静默保存，不显示任何消息
            saveDataToDisk(true);
        }
    }
}

void MiniFMS::showFileHead(Session *session, const string &fileName, int numLines)
{
    if (!session || !sharedData)
        return;

    int fileId = findFCB(session->currentDirId, fileName);
    if (fileId == -1 || sharedData->fcbs[fileId].type != 0)
    {
        cout << " 文件不存在: " << fileName << endl;
        return;
    }

    // 读取文件内容
    string content = string(sharedData->fileContents[fileId]);
    if (content.empty())
    {
        cout << " 文件为空" << endl;
        return;
    }

    // 更新访问时间
    sharedData->fcbs[fileId].accessTime = time(nullptr);

    // 分行处理
    istringstream iss(content);
    string line;
    vector<string> lines;
    while (getline(iss, line))
    {
        lines.push_back(line);
    }

    // 显示指定行数
    int linesToShow = min(numLines, static_cast<int>(lines.size()));
    cout << "\n显示 " << fileName << " 的前 " << linesToShow << " 行：\n"
         << endl;
    for (int i = 0; i < linesToShow; ++i)
    {
        cout << setw(6) << (i + 1) << " | " << lines[i] << endl;
    }
    cout << endl;
}

void MiniFMS::showFileTail(Session *session, const string &fileName, int numLines)
{
    if (!session || !sharedData)
        return;

    int fileId = findFCB(session->currentDirId, fileName);
    if (fileId == -1 || sharedData->fcbs[fileId].type != 0)
    {
        cout << " 文件不存在: " << fileName << endl;
        return;
    }

    // 读取文件内容
    string content = string(sharedData->fileContents[fileId]);
    if (content.empty())
    {
        cout << " 文件为空" << endl;
        return;
    }

    // 更新访问时间
    sharedData->fcbs[fileId].accessTime = time(nullptr);

    // 分行处理
    istringstream iss(content);
    string line;
    vector<string> lines;
    while (getline(iss, line))
    {
        lines.push_back(line);
    }

    // 显示指定行数
    int startLine = max(0, static_cast<int>(lines.size()) - numLines);
    int linesToShow = min(numLines, static_cast<int>(lines.size()));
    cout << "\n显示 " << fileName << " 的后 " << linesToShow << " 行：\n"
         << endl;
    for (int i = startLine; i < static_cast<int>(lines.size()); ++i)
    {
        cout << setw(6) << (i + 1) << " | " << lines[i] << endl;
    }
    cout << endl;
}

void MiniFMS::findAllFiles(vector<int> &files, int fcbId)
{
    if (!sharedData || fcbId < 0 || fcbId >= MAX_FCBS || !sharedData->fcbs[fcbId].isused)
    {
        cout << " 错误：无效的FCB ID: " << fcbId << endl;
        return;
    }

    files.push_back(fcbId); // 先添加当前目录/文件

    // 如果是目录，添加其中的内容
    if (sharedData->fcbs[fcbId].type == 1)
    {
        for (int i = 0; i < MAX_FCBS; i++)
        {
            if (sharedData->fcbs[i].isused && sharedData->fcbs[i].parentDir == fcbId)
            {
                files.push_back(i);
            }
        }
    }
}

void MiniFMS::deleteFCB(int fcbId)
{
    if (!sharedData || fcbId < 0 || fcbId >= MAX_FCBS || !sharedData->fcbs[fcbId].isused)
    {
        cout << " 错误：无效的FCB ID: " << fcbId << endl;
        return;
    }

    lock_guard<mutex> lock(diskMutex); // Add mutex lock for thread safety

    // 保存删除前的信息用于日志
    string itemName = sharedData->fcbs[fcbId].name;
    string itemType = sharedData->fcbs[fcbId].type == 1 ? "目录" : "文件";
    int parentDir = sharedData->fcbs[fcbId].parentDir;

    // 如果是目录，先删除其中的内容
    if (sharedData->fcbs[fcbId].type == 1)
    {
        vector<int> childrenToDelete;
        // 先收集所有子项，避免在遍历过程中修改数据
        for (int i = 0; i < MAX_FCBS; i++)
        {
            if (sharedData->fcbs[i].isused && sharedData->fcbs[i].parentDir == fcbId)
            {
                childrenToDelete.push_back(i);
            }
        }

        // 删除所有子项
        for (int childId : childrenToDelete)
        {
            string childName = sharedData->fcbs[childId].name;
            string childType = sharedData->fcbs[childId].type == 1 ? "目录" : "文件";
            cout << " - 删除" << childType << ": " << childName << endl;

            // 清理子项的FCB
            sharedData->fcbs[childId].isused = 0;
            sharedData->fcbs[childId].type = 0;
            sharedData->fcbs[childId].size = 0;
            sharedData->fcbs[childId].address = -1;
            sharedData->fcbs[childId].parentDir = -1;
            sharedData->fcbs[childId].owner = -1;
            sharedData->fcbs[childId].locked = false;
            sharedData->fcbs[childId].lockOwner = -1;
            memset(sharedData->fcbs[childId].name, 0, MAX_FILENAME_LEN);

            // 如果是文件，清空内容
            if (sharedData->fcbs[childId].type == 0)
            {
                memset(sharedData->fileContents[childId], 0, sizeof(sharedData->fileContents[childId]));
            }
        }
    }

    // 释放数据块
    int blockId = sharedData->fcbs[fcbId].address;
    while (blockId != -1 && blockId < MAX_BLOCKS)
    {
        int nextBlock = fatBlock[blockId];
        fatBlock[blockId] = 0; // 标记为空闲
        bitMap[blockId] = 0;   // 更新位图
        blockId = nextBlock;
    }

    // 从父目录中移除该FCB的引用
    if (parentDir >= 0 && parentDir < MAX_FCBS && sharedData->fcbs[parentDir].isused)
    {
        // 更新父目录的修改时间
        sharedData->fcbs[parentDir].modifyTime = time(nullptr);
        cout << " - 已从父目录 " << sharedData->fcbs[parentDir].name << " 中移除 " << itemType << ": " << itemName << endl;
    }

    // 清空当前FCB
    sharedData->fcbs[fcbId].isused = 0;
    sharedData->fcbs[fcbId].type = 0;
    sharedData->fcbs[fcbId].size = 0;
    sharedData->fcbs[fcbId].address = -1;
    sharedData->fcbs[fcbId].parentDir = -1;
    sharedData->fcbs[fcbId].owner = -1;
    sharedData->fcbs[fcbId].locked = false;
    sharedData->fcbs[fcbId].lockOwner = -1;
    memset(sharedData->fcbs[fcbId].name, 0, MAX_FILENAME_LEN);

    // 如果是文件，清空文件内容
    if (sharedData->fcbs[fcbId].type == 0)
    {
        memset(sharedData->fileContents[fcbId], 0, sizeof(sharedData->fileContents[fcbId]));
    }

    // 标记数据已修改
    sharedData->modifyCount++;
    dataChanged = true;
}

// 导入外部文件到文件系统
bool MiniFMS::importFile(Session *session, const string &externalPath, const string &internalName)
{
    // 检查文件是否已存在
    if (findFCB(session->currentDirId, internalName) != -1)
    {
        cout << " 错误：文件已存在：" << internalName << endl;
        return false;
    }

    // 读取外部文件
    ifstream inFile(externalPath, ios::binary);
    if (!inFile.is_open())
    {
        cout << " 错误：无法打开外部文件：" << externalPath << endl;
        return false;
    }

    // 读取文件内容
    stringstream buffer;
    buffer << inFile.rdbuf();
    string content = buffer.str();
    inFile.close();

    // 创建新文件
    int newFileId = createFCB(internalName, 0, session->user->userId, session->currentDirId);
    if (newFileId == -1)
    {
        cout << " 错误：创建文件失败" << endl;
        return false;
    }

    // 写入文件内容
    if (content.length() >= sizeof(sharedData->fileContents[newFileId]))
    {
        cout << " 错误：文件太大，超出系统限制" << endl;
        deleteFCB(newFileId);
        return false;
    }

    strncpy(sharedData->fileContents[newFileId], content.c_str(), sizeof(sharedData->fileContents[newFileId]) - 1);
    sharedData->fcbs[newFileId].size = content.length();
    sharedData->fcbs[newFileId].modifyTime = time(nullptr);

    cout << " 文件导入成功：" << internalName << endl;
    cout << " - 大小：" << content.length() << " 字节" << endl;
    cout << " - 修改时间：" << formatTime(sharedData->fcbs[newFileId].modifyTime) << endl;

    sharedData->modifyCount++;
    dataChanged = true;
    return true;
}

// 导出文件到外部文件系统
bool MiniFMS::exportFile(Session *session, const string &internalName, const string &externalPath)
{
    // 查找文件
    int fileId = findFCB(session->currentDirId, internalName);
    if (fileId == -1 || sharedData->fcbs[fileId].type != 0)
    {
        cout << " 错误：文件不存在：" << internalName << endl;
        return false;
    }

    // 检查文件访问权限
    if (!checkFileAccess(session, fileId, false))
    {
        return false;
    }

    // 写入外部文件
    ofstream outFile(externalPath, ios::binary);
    if (!outFile.is_open())
    {
        cout << " 错误：无法创建外部文件：" << externalPath << endl;
        return false;
    }

    // 获取文件内容
    string content = string(sharedData->fileContents[fileId]);
    outFile.write(content.c_str(), sharedData->fcbs[fileId].size);
    outFile.close();

    cout << " 文件导出成功：" << internalName << " -> " << externalPath << endl;
    cout << " - 大小：" << sharedData->fcbs[fileId].size << " 字节" << endl;
    cout << " - 修改时间：" << formatTime(sharedData->fcbs[fileId].modifyTime) << endl;

    // 更新访问时间
    sharedData->fcbs[fileId].accessTime = time(nullptr);
    dataChanged = true;
    return true;
}

int main()
{
#ifdef _WIN32
    // 设置控制台编码为UTF-8
    system("chcp 65001 >nul");
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
#endif

    try
    {
        MiniFMS fms; // 创建系统对象
        fms.run();   // 运行系统
    }
    catch (const exception &e)
    {
        cerr << " 系统错误: " << e.what() << endl;
        return 1;
    }

    return 0;
}