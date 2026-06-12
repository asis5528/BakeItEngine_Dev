#include "QuestDeploy.h"
#include <cstdio>
#include <cstdlib>

namespace
{
std::string envOr(const char* name, const char* fallback)
{
	char* v = nullptr;
	size_t len = 0;
	std::string result(fallback);
	if (_dupenv_s(&v, &len, name) == 0 && v && v[0]) {
		result = v;
	}
	std::free(v);
	return result;
}

// Defaults match the CLI toolchain documented in Android/README.md; override
// with the env vars when the installs live elsewhere.
std::string resolveJavaHome()
{
	return envOr("JAVA_HOME", "C:\\Program Files\\Eclipse Adoptium\\jdk-17.0.19.10-hotspot");
}
std::string resolveAndroidHome()
{
	return envOr("ANDROID_HOME", "C:\\Android");
}
std::string resolveGradleBat()
{
	return envOr("BAKEIT_GRADLE_BAT", "C:\\Gradle\\gradle-8.7\\bin\\gradle.bat");
}
std::string resolveAdb()
{
	return resolveAndroidHome() + "\\platform-tools\\adb.exe";
}
} // namespace

QuestDeploy::~QuestDeploy()
{
	if (m_thread.joinable()) {
		m_thread.join();
	}
}

void QuestDeploy::start()
{
	if (m_running.exchange(true)) {
		return; // already running
	}
	if (m_thread.joinable()) {
		m_thread.join(); // reap the previous run
	}
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_log.clear();
	}
	m_thread = std::thread([this]() { worker(); });
}

std::string QuestDeploy::status()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_status;
}

std::vector<std::string> QuestDeploy::logTail(size_t maxLines)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if (m_log.size() <= maxLines) {
		return m_log;
	}
	return std::vector<std::string>(m_log.end() - maxLines, m_log.end());
}

void QuestDeploy::setStatus(const std::string& s)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_status = s;
}

void QuestDeploy::appendLog(const std::string& line)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_log.push_back(line);
	if (m_log.size() > 400) {
		m_log.erase(m_log.begin(), m_log.begin() + 100);
	}
}

// Runs `command` through cmd with merged stderr, streaming output into the
// log. Returns true when the process exits 0.
bool QuestDeploy::runStep(const std::string& label, const std::string& command)
{
	setStatus(label);
	appendLog("== " + label);

	const std::string full = "cmd /c \"" + command + "\" 2>&1";
	FILE* pipe = _popen(full.c_str(), "r");
	if (!pipe) {
		appendLog("failed to launch: " + command);
		return false;
	}
	char buffer[512];
	while (std::fgets(buffer, sizeof(buffer), pipe)) {
		std::string line(buffer);
		while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
			line.pop_back();
		}
		if (!line.empty()) {
			appendLog(line);
		}
	}
	const int exitCode = _pclose(pipe);
	if (exitCode != 0) {
		appendLog(label + " FAILED (exit " + std::to_string(exitCode) + ")");
		return false;
	}
	return true;
}

void QuestDeploy::worker()
{
	const std::string javaHome = resolveJavaHome();
	const std::string androidHome = resolveAndroidHome();
	const std::string gradle = resolveGradleBat();
	const std::string adb = resolveAdb();
	// Inherited by every step; gradle needs both.
	const std::string envPrefix =
		"set \"JAVA_HOME=" + javaHome + "\" && set \"ANDROID_HOME=" + androidHome + "\" && ";

	bool ok =
		// Working dir is Source/ - same script the engine runs at startup.
		runStep("compiling shaders...", "compile.bat") &&
		runStep("building APK...",
			envPrefix + "cd /d \"..\\Android\" && \"" + gradle + "\" assembleDebug --no-daemon") &&
		runStep("installing on headset...",
			"\"" + adb + "\" install -r \"..\\Android\\app\\build\\outputs\\apk\\debug\\app-debug.apk\"") &&
		runStep("launching...",
			"\"" + adb + "\" shell am force-stop com.bakeit.questdemo && \"" + adb +
			"\" shell am start -n com.bakeit.questdemo/android.app.NativeActivity");

	setStatus(ok ? "DONE - running on headset" : "FAILED - see log");
	m_running.store(false);
}
