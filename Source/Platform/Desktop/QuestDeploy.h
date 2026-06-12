#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Desktop-only: builds and deploys the Quest APK from inside the editor.
// One click runs, on a background thread (the editor keeps rendering):
//   1. compile.bat            - fresh .spv for the APK assets
//   2. gradle assembleDebug   - Android/ project (engine sources compile in)
//   3. adb install -r         - push to the connected headset
//   4. adb shell am start     - relaunch the app
// Output of every step streams into the log shown by EditorGui's Quest panel.
//
// Toolchain discovery: JAVA_HOME / ANDROID_HOME / BAKEIT_GRADLE_BAT env vars
// when set, otherwise this dev machine's install paths (see resolve* in the
// .cpp). Working directory must be Source/ (the engine guarantees that).
class QuestDeploy
{
public:
	~QuestDeploy();

	// Kick off a build+deploy; ignored while one is already running.
	void start();

	bool isRunning() const { return m_running.load(); }

	// One-line state for the UI ("idle", "building APK...", "DONE", ...).
	std::string status();

	// Most recent log lines (newest last).
	std::vector<std::string> logTail(size_t maxLines = 12);

private:
	void worker();
	bool runStep(const std::string& label, const std::string& command);
	void setStatus(const std::string& s);
	void appendLog(const std::string& line);

	std::thread m_thread;
	std::atomic<bool> m_running{ false };
	mutable std::mutex m_mutex;
	std::string m_status = "idle";
	std::vector<std::string> m_log;
};
