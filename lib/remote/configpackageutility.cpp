/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "remote/configpackageutility.hpp"
#include "remote/apilistener.hpp"
#include "base/application.hpp"
#include "base/atomic-file.hpp"
#include "base/exception.hpp"
#include "base/utility.hpp"
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>

using namespace icinga;

String ConfigPackageUtility::GetPackageDir()
{
	return Configuration::DataDir + "/api/packages";
}

void ConfigPackageUtility::CreatePackage(const String& name)
{
	String path = GetPackageDir() + "/" + name;

	if (Utility::PathExists(path))
		BOOST_THROW_EXCEPTION(std::invalid_argument("Package already exists."));

	Utility::MkDirP(path, 0700);
	WritePackageConfig(name);
}

void ConfigPackageUtility::DeletePackage(const String& name)
{
	String path = GetPackageDir() + "/" + name;

	if (!Utility::PathExists(path))
		BOOST_THROW_EXCEPTION(std::invalid_argument("Package does not exist."));

	ApiListener::Ptr listener = ApiListener::GetInstance();

	/* config packages without API make no sense. */
	if (!listener)
		BOOST_THROW_EXCEPTION(std::invalid_argument("No ApiListener instance configured."));

	listener->RemoveActivePackageStage(name);

	Utility::RemoveDirRecursive(path);
	Application::RequestRestart();
}

std::vector<String> ConfigPackageUtility::GetPackages()
{
	String packageDir = GetPackageDir();

	std::vector<String> packages;

	/* Package directory does not exist, no packages have been created thus far. */
	if (!Utility::PathExists(packageDir))
		return packages;

	Utility::Glob(packageDir + "/*", [&packages](const String& path) { packages.emplace_back(Utility::BaseName(path)); }, GlobDirectory);

	return packages;
}

bool ConfigPackageUtility::PackageExists(const String& name)
{
	auto packages (GetPackages());
	return std::find(packages.begin(), packages.end(), name) != packages.end();
}

String ConfigPackageUtility::CreateStage(const String& packageName, const Dictionary::Ptr& files)
{
	String stageName = Utility::NewUniqueID();

	String path = GetPackageDir() + "/" + packageName;

	if (!Utility::PathExists(path))
		BOOST_THROW_EXCEPTION(std::invalid_argument("Package does not exist."));

	path += "/" + stageName;

	Utility::MkDirP(path, 0700);
	Utility::MkDirP(path + "/conf.d", 0700);
	Utility::MkDirP(path + "/zones.d", 0700);
	WriteStageConfig(packageName, stageName);

	bool foundDotDot = false;

	if (files) {
		ObjectLock olock(files);
		for (const Dictionary::Pair& kv : files) {
			if (ContainsDotDot(kv.first)) {
				foundDotDot = true;
				break;
			}

			String filePath = path + "/" + kv.first;

			Log(LogInformation, "ConfigPackageUtility")
				<< "Updating configuration file: " << filePath;

			// Pass the directory and generate a dir tree, if it does not already exist
			Utility::MkDirP(Utility::DirName(filePath), 0750);
			std::ofstream fp(filePath.CStr(), std::ofstream::out | std::ostream::binary | std::ostream::trunc);
			fp << kv.second;
			fp.close();
		}
	}

	if (foundDotDot) {
		Utility::RemoveDirRecursive(path);
		BOOST_THROW_EXCEPTION(std::invalid_argument("Path must not contain '..'."));
	}

	return stageName;
}

void ConfigPackageUtility::WritePackageConfig(const String& packageName)
{
	String stageName = GetActiveStage(packageName);
	AtomicFile::Write(GetPackageDir() + "/" + packageName + "/include.conf", 0644, "include \"*/include.conf\"\n");

	AtomicFile fpActive(GetPackageDir() + "/" + packageName + "/active.conf", 0644);
	fpActive << "if (!globals.contains(\"ActiveStages\")) {\n"
		<< "  globals.ActiveStages = {}\n"
		<< "}\n"
		<< "\n"
		<< "if (globals.contains(\"ActiveStageOverride\")) {\n"
		<< "  var arr = ActiveStageOverride.split(\":\")\n"
		<< "  if (arr[0] == \"" << packageName << "\") {\n"
		<< "    if (arr.len() < 2) {\n"
		<< "      log(LogCritical, \"Config\", \"Invalid value for ActiveStageOverride\")\n"
		<< "    } else {\n"
		<< "      ActiveStages[\"" << packageName << "\"] = arr[1]\n"
		<< "    }\n"
		<< "  }\n"
		<< "}\n"
		<< "\n"
		<< "if (!ActiveStages.contains(\"" << packageName << "\")) {\n"
		<< "  ActiveStages[\"" << packageName << "\"] = \"" << stageName << "\"\n"
		<< "}\n";
	fpActive.Commit();
}

void ConfigPackageUtility::WriteStageConfig(const String& packageName, const String& stageName)
{
	AtomicFile fp(GetPackageDir() + "/" + packageName + "/" + stageName + "/include.conf", 0644);
	fp << "include \"../active.conf\"\n"
		<< "if (ActiveStages[\"" << packageName << "\"] == \"" << stageName << "\") {\n"
		<< "  include_recursive \"conf.d\"\n"
		<< "  include_zones \"" << packageName << "\", \"zones.d\"\n"
		<< "}\n";
	fp.Commit();
}

void ConfigPackageUtility::ActivateStage(const String& packageName, const String& stageName)
{
	SetActiveStage(packageName, stageName);

	WritePackageConfig(packageName);
}

void ConfigPackageUtility::TryActivateStageCallback(const ProcessResult& pr, const String& packageName, const String& stageName,
	bool activate, bool reload, const Shared<Defer>::Ptr& resetPackageUpdates)
{
	String logFile = GetPackageDir() + "/" + packageName + "/" + stageName + "/startup.log";
	std::ofstream fpLog(logFile.CStr(), std::ofstream::out | std::ostream::binary | std::ostream::trunc);
	fpLog << pr.Output;
	fpLog.close();

	String statusFile = GetPackageDir() + "/" + packageName + "/" + stageName + "/status";
	std::ofstream fpStatus(statusFile.CStr(), std::ofstream::out | std::ostream::binary | std::ostream::trunc);
	fpStatus << pr.ExitStatus;
	fpStatus.close();

	/* validation went fine, activate stage and reload */
	if (pr.ExitStatus == 0) {
		if (activate) {
			{
				std::unique_lock<std::mutex> lock(GetStaticPackageMutex());

				ActivateStage(packageName, stageName);
			}

			if (reload) {
				/*
				 * Cancel the deferred callback before going out of scope so that the config stages handler
				 * flag isn't resetting earlier and allowing other clients to submit further requests while
				 * Icinga2 is reloading. Otherwise, the ongoing request will be cancelled halfway before the
				 * operation is completed once the new worker becomes ready.
				 */
				resetPackageUpdates->Cancel();

				Application::RequestRestart();
			}
		}
	} else {
		Log(LogCritical, "ConfigPackageUtility")
			<< "Config validation failed for package '"
			<< packageName << "' and stage '" << stageName << "'.";
	}
}

void ConfigPackageUtility::AsyncTryActivateStage(const String& packageName, const String& stageName, bool activate, bool reload,
	const Shared<Defer>::Ptr& resetPackageUpdates)
{
	VERIFY(Application::GetArgC() >= 1);

	// prepare arguments
	Array::Ptr args = new Array({
		Application::GetExePath(Application::GetArgV()[0]),
	});

	// copy all arguments of parent process
	for (int i = 1; i < Application::GetArgC(); i++) {
		String argV = Application::GetArgV()[i];

		if (argV == "-d" || argV == "--daemonize")
			continue;

		args->Add(argV);
	}

	// add arguments for validation
	args->Add("--validate");
	args->Add("--define");
	args->Add("ActiveStageOverride=" + packageName + ":" + stageName);

	Process::Ptr process = new Process(Process::PrepareCommand(args));
	process->SetTimeout(Application::GetReloadTimeout());
	process->Run([packageName, stageName, activate, reload, resetPackageUpdates](const ProcessResult& pr) {
		TryActivateStageCallback(pr, packageName, stageName, activate, reload, resetPackageUpdates);
	});
}

void ConfigPackageUtility::DeleteStage(const String& packageName, const String& stageName)
{
	String path = GetPackageDir() + "/" + packageName + "/" + stageName;

	if (!Utility::PathExists(path))
		BOOST_THROW_EXCEPTION(std::invalid_argument("Stage does not exist."));

	if (GetActiveStage(packageName) == stageName)
		BOOST_THROW_EXCEPTION(std::invalid_argument("Active stage cannot be deleted."));

	Utility::RemoveDirRecursive(path);
}

std::vector<String> ConfigPackageUtility::GetStages(const String& packageName)
{
	std::vector<String> stages;
	Utility::Glob(GetPackageDir() + "/" + packageName + "/*", [&stages](const String& path) { stages.emplace_back(Utility::BaseName(path)); }, GlobDirectory);
	return stages;
}

String ConfigPackageUtility::GetActiveStageFromFile(const String& packageName)
{
	/* Lock the transaction, reading this only happens on startup or when something really is broken. */
	std::unique_lock<std::mutex> lock(GetStaticActiveStageMutex());

	String path = GetPackageDir() + "/" + packageName + "/active-stage";

	std::ifstream fp;
	fp.open(path.CStr());

	String stage;
	std::getline(fp, stage.GetData());

	fp.close();

	if (fp.fail())
		return ""; /* Don't use exceptions here. The caller must deal with empty stages at this point. Happens on initial package creation for example. */

	return stage.Trim();
}

void ConfigPackageUtility::SetActiveStageToFile(const String& packageName, const String& stageName)
{
	std::unique_lock<std::mutex> lock(GetStaticActiveStageMutex());
	AtomicFile::Write(GetPackageDir() + "/" + packageName + "/active-stage", 0644, stageName);
}

String ConfigPackageUtility::GetActiveStage(const String& packageName)
{
	String activeStage;

	ApiListener::Ptr listener = ApiListener::GetInstance();

	/* If we don't have an API feature, just use the file storage without caching this.
	 * This happens when ScheduledDowntime objects generate Downtime objects.
	 * TODO: Make the API a first class citizen.
	 */
	if (!listener)
		return GetActiveStageFromFile(packageName);

	/* First use runtime state. */
	try {
		activeStage = listener->GetActivePackageStage(packageName);
	} catch (const std::exception& ex) {
		/* Fallback to reading the file, happens on restarts. */
		activeStage = GetActiveStageFromFile(packageName);

		/* When we've read something, correct memory. */
		if (!activeStage.IsEmpty())
			listener->SetActivePackageStage(packageName, activeStage);
	}

	return activeStage;
}

void ConfigPackageUtility::SetActiveStage(const String& packageName, const String& stageName)
{
	/* Update the marker on disk for restarts. */
	SetActiveStageToFile(packageName, stageName);

	ApiListener::Ptr listener = ApiListener::GetInstance();

	/* No API, no caching. */
	if (!listener)
		return;

	listener->SetActivePackageStage(packageName, stageName);
}

std::vector<std::pair<String, bool> > ConfigPackageUtility::GetFiles(const String& packageName, const String& stageName)
{
	std::vector<std::pair<String, bool> > paths;
	Utility::GlobRecursive(GetPackageDir() + "/" + packageName + "/" + stageName, "*", [&paths](const String& path) {
		CollectPaths(path, paths);
	}, GlobDirectory | GlobFile);

	return paths;
}

void ConfigPackageUtility::CollectPaths(const String& path, std::vector<std::pair<String, bool> >& paths)
{
#ifndef _WIN32
	struct stat statbuf;
	int rc = lstat(path.CStr(), &statbuf);
	if (rc < 0)
		BOOST_THROW_EXCEPTION(posix_error()
			<< boost::errinfo_api_function("lstat")
			<< boost::errinfo_errno(errno)
			<< boost::errinfo_file_name(path));

	paths.emplace_back(path, S_ISDIR(statbuf.st_mode));
#else /* _WIN32 */
	struct _stat statbuf;
	int rc = _stat(path.CStr(), &statbuf);
	if (rc < 0)
		BOOST_THROW_EXCEPTION(posix_error()
			<< boost::errinfo_api_function("_stat")
			<< boost::errinfo_errno(errno)
			<< boost::errinfo_file_name(path));

	paths.emplace_back(path, ((statbuf.st_mode & S_IFMT) == S_IFDIR));
#endif /* _WIN32 */
}

bool ConfigPackageUtility::ContainsDotDot(const String& path)
{
	std::vector<String> tokens = path.Split("/\\");

	for (const String& part : tokens) {
		if (part == "..")
			return true;
	}

	return false;
}

bool ConfigPackageUtility::ValidatePackageName(const String& packageName)
{
	return ValidateFreshName(packageName) || PackageExists(packageName);
}

bool ConfigPackageUtility::ValidateFreshName(const String& name)
{
	if (name.IsEmpty())
		return false;

	/* check for path injection */
	if (ContainsDotDot(name))
		return false;

	return std::all_of(name.Begin(), name.End(), [](char c) {
		return std::isalnum(c, std::locale::classic()) || c == '_' || c == '-';
	});
}

std::mutex& ConfigPackageUtility::GetStaticPackageMutex()
{
	static std::mutex mutex;
	return mutex;
}

std::mutex& ConfigPackageUtility::GetStaticActiveStageMutex()
{
	static std::mutex mutex;
	return mutex;
}
