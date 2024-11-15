#define NOMINMAX
#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <iostream>
#include "vfs/VirtualFileSystem.h"
#include "vfs/native/NativeFileSystem.h"
#include "vfs/memory/MemoryFileSystem.h"
#include "vfs/pack/PackFileSystem.h"

#include "md5/md5.h"

USING_NS_VFS;

VirtualFileSystem virtualFileSystem;

void mountListWindow()
{
    ImGui::SetNextWindowPos(ImVec2(100, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("mount list");

    static char pathBuf[1024] = { 0 };
    static char mntpointBuf[1024] = { 0 };

    ImGui::InputText("path", pathBuf, sizeof(pathBuf));
    ImGui::InputText("mntpoint", mntpointBuf, sizeof(mntpointBuf));
    if (ImGui::Button("mount native"))
    {
        if (virtualFileSystem.mount(new NativeFileSystem(pathBuf, mntpointBuf)))
            printf("mount success\n");
        else
            printf("mount failed\n");
    }
    ImGui::SameLine();
    if (ImGui::Button("mount memory"))
    {
        if (virtualFileSystem.mount(new MemoryFileSystem(pathBuf, mntpointBuf)))
            printf("mount success\n");
        else
            printf("mount failed\n");
    }
    ImGui::SameLine();
    if (ImGui::Button("mount pack file"))
    {
        if (virtualFileSystem.mount(new PackFileSystem(pathBuf, mntpointBuf)))
            printf("mount success\n");
        else
            printf("mount failed\n");
    }

    ImGui::Separator();

    ImGui::Text("mount list:");
    if (ImGui::BeginChild("mount list", ImVec2(0, 0), true))
    {
        int id = 0;
        for (auto& fs : virtualFileSystem.getFileSystems())
        {
            ImGui::PushID(id++);
            ImGui::Text("archive location: %s", fs->archiveLocation().data());
            ImGui::Text("mount point: %s", fs->mntpoint().data());
            switch (fs->getFileSystemType())
            {
            case FileSystemType::Native:
                ImGui::Text("file system type: native");
                break;
            case FileSystemType::Memory:
                ImGui::Text("file system type: memory");
                break;
            case FileSystemType::PackFile:
                ImGui::Text("file system type: pack file");
                break;
            default:
                ImGui::Text("file system type: unknown");
                break;
            }

            if (ImGui::Button("unmount"))
            {
                virtualFileSystem.unmount(fs);
            }
            ImGui::PopID();
            ImGui::Separator();
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

bool readFile(VirtualFileSystem& virtualFileSystem, const std::string& fileName, bool printContent)
{
    bool ok = false;
    printf("\n\n>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %s <<<<<<<<<<<<<<<<<<<<<<<<<<< read begin\n", fileName.c_str());
    auto stream = virtualFileSystem.openFileStream(fileName, FileStream::Mode::READ);
    if (stream)
    {
        auto dataLen = stream->size();
        std::vector<uint8_t> buf;
        buf.resize(dataLen);

        const size_t MAX_READ_LEN = 1024 * 10;
        size_t index = 0;
        while (index < dataLen)
        {
            auto readLen = std::min(MAX_READ_LEN, dataLen - index);
            auto realReadLen = stream->read((buf.data() + index), MAX_READ_LEN);
            assert(readLen == realReadLen);
            index += readLen;

            printf("stream tell: %llu\n", stream->tell());
        }

        std::string str = std::string((char*)buf.data(), buf.size());
        printf("file size: %llu\n", dataLen);
        printf("file md5: %s\n", md5(str).c_str());
        if (printContent)
            printf("%s\n", str.c_str());

        ok = true;
    }
    else
    {
        printf("no file: %s\n", fileName.c_str());
    }
    printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> %s <<<<<<<<<<<<<<<<<<<<<<<<<<< read end\n\n", fileName.c_str());

    return ok;
}

void fileListWindow()
{
    ImGui::SetNextWindowPos(ImVec2(600, 100), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(400, 400), ImGuiCond_FirstUseEver);
    ImGui::Begin("file list");

    static std::string curDirPath = "/";


    if (virtualFileSystem.isDir(curDirPath))
    {
        if (ImGui::Button("/"))
            curDirPath = "/";
        if (!curDirPath.empty() && curDirPath != "/")
        {
            std::vector<std::string> parts = splitString(curDirPath.substr(1, curDirPath.size() - 2), "/");

            if(!parts.empty())
                ImGui::SameLine();

            for (size_t i = 0; i < parts.size(); ++i)
            {
                ImGui::PushID(i);

                if (ImGui::Button(parts[i].c_str()))
                {
                    curDirPath = "/";
                    for (size_t j = 0; j <= i; ++j)
                    {
                        curDirPath += parts[j];
                        curDirPath += "/";
                    }
                    curDirPath = convertDirPath(curDirPath);
                }

                if (i != parts.size() - 1)
                    ImGui::SameLine();

                ImGui::PopID();
            }
        }

        if (ImGui::BeginPopupContextItem("new_popup"))
        {
            static char buf[1024] = { 0 };
            ImGui::InputText("name", buf, sizeof(buf));

            if (ImGui::Button("new file"))
            {
                ImGui::CloseCurrentPopup();
                auto fileName = curDirPath + buf;
                if (virtualFileSystem.isFile(fileName))
                {
                    printf("the file %s already exists\n", fileName.c_str());
                }
                else
                {
                    auto fs = virtualFileSystem.openFileStream(fileName, FileStream::Mode::WRITE);
                    if (fs)
                    {
                        std::string text = "My path is: " + fileName;
                        fs->write(text.data(), text.length());
                        printf("create file(%s) success\n", fileName.c_str());
                    }
                    else
                    {
                        printf("create file(%s) failed\n", fileName.c_str());
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("new dir"))
            {
                ImGui::CloseCurrentPopup();
                auto dirName = curDirPath + buf;
                if (virtualFileSystem.isDir(dirName))
                {
                    printf("the dir %s already exists\n", dirName.c_str());
                }
                else
                {
                    printf("create dir(%s) %s\n", dirName.c_str(), virtualFileSystem.createDir(dirName) ? "success" : "failed");
                }
            }

            ImGui::EndPopup();
        }

        ImGui::Separator();
        if (ImGui::Button("new"))
        {
            ImGui::OpenPopup("new_popup");
        }
        ImGui::Separator();

        if (ImGui::BeginChild("file list", ImVec2(0, 0), true))
        {
            static ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersV | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;
            ImVec2 outer_size = ImVec2(0.0f, 0);
            if (ImGui::BeginTable("table_scrollx", 3, flags, outer_size))
            {
                ImGui::TableSetupColumn("permissions");
                ImGui::TableSetupColumn("type");
                ImGui::TableSetupColumn("name");
                ImGui::TableHeadersRow();

                virtualFileSystem.enumerate(curDirPath, [&](const FileInfo& fileInfo) -> bool {
                    ImGui::TableNextRow();

                    int column = 0;

                    ImGui::TableSetColumnIndex(column++);
                    if (fileInfo.flags & FileFlags::Read)
                    {
                        if (fileInfo.flags & FileFlags::Write)
                            ImGui::Text("rw");
                        else
                            ImGui::Text("r");
                    }
                    else
                    {
                        ImGui::Text("err");
                    }

                    ImGui::TableSetColumnIndex(column++);
                    if (fileInfo.flags & FileFlags::Dir)
                        ImGui::Text("dir");
                    else
                        ImGui::Text("file");

                    ImGui::TableSetColumnIndex(column++);
                    if (ImGui::Button(fileInfo.filePath.c_str()))
                    {
                        if (fileInfo.flags & FileFlags::Dir)
                        {
                            curDirPath = fileInfo.filePath;
                            curDirPath = convertDirPath(curDirPath);
                        }
                        else
                        {
                            bool showContent = fileInfo.filePath.ends_with(".txt") || fileInfo.filePath.ends_with(".py");
                            readFile(virtualFileSystem, fileInfo.filePath, showContent);
                        }
                    }

                    return false;
                });

                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }
    else
    {
        curDirPath = "/";
    }

    ImGui::End();
}

void imguiDraw()
{
    mountListWindow();
    fileListWindow();

    //ImGui::ShowDemoWindow();
}

int main()
{
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    const GLFWvidmode* videoMode = glfwGetVideoMode(monitor);
    GLFWwindow* window = glfwCreateWindow(videoMode->width * 0.6f, videoMode->height * 0.8f, "fnt_creator", NULL, NULL);
    if (window == NULL)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }

    glfwSetWindowSizeLimits(window, 200, 150, GLFW_DONT_CARE, GLFW_DONT_CARE);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return -1;
    }

    // Dear ImGui
    const char* glsl_version = "#version 330";
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    while (!glfwWindowShouldClose(window))
    {
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);

        glViewport(0, 0, display_w, display_h);

        glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);

        // Dear ImGui
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        imguiDraw();

        // Rendering
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwTerminate();
    return 0;
}
