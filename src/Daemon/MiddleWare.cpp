/*
    MiddleWare.cpp

    Copyright (C) 2009  Zdenek Prikryl (zprikryl@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    */

#include "abrtlib.h"
#include "abrt_types.h"
#include "Daemon.h"
#include "Settings.h"
#include "RPM.h"
#include "DebugDump.h"
#include "ABRTException.h"
#include "CommLayerInner.h"
#include "MiddleWare.h"


/**
 * An instance of CPluginManager. When MiddleWare wants to do something
 * with plugins, it calls the plugin manager.
 * @see PluginManager.h
 */
CPluginManager* g_pPluginManager;
/**
 * A set of blacklisted packages.
 */
set_string_t g_setBlackList;
/**
 * An instance of CRPM used for package checking.
 * @see RPM.h
 */
static CRPM s_RPM;


/**
 * A map, which associates particular analyzer to one or more
 * action or reporter plugins. These are activated when a crash, which
 * is maintained by particular analyzer, occurs.
 */
typedef std::map<std::string, vector_pair_string_string_t> map_analyzer_actions_and_reporters_t;
static map_analyzer_actions_and_reporters_t s_mapAnalyzerActionsAndReporters;
/**
 * A vector of one or more action or reporter plugins. These are
 * activated when any crash occurs.
 */
static vector_pair_string_string_t s_vectorActionsAndReporters;


static void RunAnalyzerActions(const char *pAnalyzer, const char *pDebugDumpDir);


static char* is_text_file(const char *name, ssize_t *sz)
{
    /* We were using magic.h API to check for file being text, but it thinks
     * that file containing just "0" is not text (!!)
     * So, we do it ourself.
     */

    int fd = open(name, O_RDONLY);
    if (fd < 0)
        return NULL; /* it's not text (because it does not exist! :) */

    char *buf = (char*)xmalloc(*sz);
    ssize_t r = *sz = full_read(fd, buf, *sz);
    close(fd);
    if (r < 0)
    {
        free(buf);
        return NULL; /* it's not text (because we can't read it) */
    }

    /* Some files in our dump directories are known to always be textual */
    if (strcmp(name, FILENAME_BACKTRACE) == 0
     || strcmp(name, FILENAME_CMDLINE) == 0
    ) {
        return buf;
    }

    /* Every once in a while, even a text file contains a few garbled
     * or unexpected non-ASCII chars. We should not declare it "binary".
     */
    const unsigned RATIO = 50;
    unsigned total_chars = r + RATIO;
    unsigned bad_chars = 1; /* 1 prevents division by 0 later */
    while (--r >= 0)
    {
        if (buf[r] >= 0x7f
         /* among control chars, only '\t','\n' etc are allowed */
         || (buf[r] < ' ' && !isspace(buf[r]))
        ) {
            if (buf[r] == '\0')
            {
                /* We don't like NULs very much. Not text for sure! */
                free(buf);
                return NULL;
            }
            bad_chars++;
        }
    }

    if ((total_chars / bad_chars) >= RATIO)
        return buf; /* looks like text to me */

    free(buf);
    return NULL; /* it's binary */
}

static void load_crash_data_from_debug_dump(CDebugDump& dd, map_crash_data_t& data)
{
    std::string short_name;
    std::string full_name;

    dd.InitGetNextFile();
    while (dd.GetNextFile(&short_name, &full_name))
    {
        ssize_t sz = 4*1024;
        char *text = is_text_file(full_name.c_str(), &sz);
        if (!text)
        {
            add_to_crash_data_ext(data,
                    short_name.c_str(),
                    CD_BIN,
                    CD_ISNOTEDITABLE,
                    full_name.c_str()
            );
            continue;
        }

        std::string content;
        if (sz < 4*1024) /* is_text_file did read entire file */
            content.assign(text, sz);
        else /* no, need to read it all */
            dd.LoadText(short_name.c_str(), content);
        free(text);

        add_to_crash_data_ext(data,
                short_name.c_str(),
                CD_TXT,
                is_editable_file(short_name.c_str()) ? CD_ISEDITABLE : CD_ISNOTEDITABLE,
                content.c_str()
        );
    }
}

/**
 * Transforms a debugdump directory to inner crash
 * report form. This form is used for later reporting.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @param pCrashData A created crash report.
 */
static void DebugDumpToCrashReport(const char *pDebugDumpDir, map_crash_data_t& pCrashData)
{
    VERB3 log(" DebugDumpToCrashReport('%s')", pDebugDumpDir);

    CDebugDump dd;
    dd.Open(pDebugDumpDir);

    const char *const *v = must_have_files;
    while (*v)
    {
        if (!dd.Exist(*v))
        {
            throw CABRTException(EXCEP_ERROR, "DebugDumpToCrashReport(): important file '%s' is missing", *v);
        }
        v++;
    }

    load_crash_data_from_debug_dump(dd, pCrashData);
}

/**
 * Get a local UUID from particular analyzer plugin.
 * @param pAnalyzer A name of an analyzer plugin.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @return A local UUID.
 */
static std::string GetLocalUUID(const char *pAnalyzer, const char *pDebugDumpDir)
{
    CAnalyzer* analyzer = g_pPluginManager->GetAnalyzer(pAnalyzer);
    if (analyzer)
    {
        return analyzer->GetLocalUUID(pDebugDumpDir);
    }
    throw CABRTException(EXCEP_PLUGIN, "Error running '%s'", pAnalyzer);
}

/**
 * Get a global UUID from particular analyzer plugin.
 * @param pAnalyzer A name of an analyzer plugin.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @return A global UUID.
 */
static std::string GetGlobalUUID(const char *pAnalyzer,
                                       const char *pDebugDumpDir)
{
    CAnalyzer* analyzer = g_pPluginManager->GetAnalyzer(pAnalyzer);
    if (analyzer)
    {
        return analyzer->GetGlobalUUID(pDebugDumpDir);
    }
    throw CABRTException(EXCEP_PLUGIN, "Error running '%s'", pAnalyzer);
}

/**
 * Take care of getting all additional data needed
 * for computing UUIDs and creating a report for particular analyzer
 * plugin. This report could be send somewhere afterwards.
 * @param pAnalyzer A name of an analyzer plugin.
 * @param pDebugDumpPath A debugdump dir containing all necessary data.
 */
static void run_analyser_CreateReport(const char *pAnalyzer,
                const char *pDebugDumpDir,
                int force)
{
    CAnalyzer* analyzer = g_pPluginManager->GetAnalyzer(pAnalyzer);
    if (analyzer)
    {
        analyzer->CreateReport(pDebugDumpDir, force);
    }
    /* else: GetAnalyzer() already complained, no need to handle it here */
}

mw_result_t CreateCrashReport(const char *pUUID,
                const char *pUID,
                int force,
                map_crash_data_t& pCrashData)
{
    VERB2 log("CreateCrashReport('%s','%s',result)", pUUID, pUID);

    database_row_t row;
    if (pUUID[0] != '\0')
    {
        CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
        database->Connect();
        row = database->GetRow(pUUID, pUID);
        database->DisConnect();
    }
    if (pUUID[0] == '\0' || row.m_sUUID != pUUID)
    {
        error_msg("UUID '%s' is not in database", pUUID);
        return MW_IN_DB_ERROR;
    }

    mw_result_t r = MW_OK;
    try
    {
        {
            CDebugDump dd;
            dd.Open(row.m_sDebugDumpDir.c_str());
            load_crash_data_from_debug_dump(dd, pCrashData);
        }

        std::string analyzer = get_crash_data_item_content(pCrashData, FILENAME_ANALYZER);

        // TODO: explain what run_analyser_CreateReport and RunAnalyzerActions are expected to do.
        // Do they potentially add more files to dump dir?
        // Why we calculate dup_hash after run_analyser_CreateReport but before RunAnalyzerActions?
        // Why do we reload dump dir's data via DebugDumpToCrashReport?

        VERB3 log(" run_analyser_CreateReport('%s')", analyzer.c_str());
        run_analyser_CreateReport(analyzer.c_str(), row.m_sDebugDumpDir.c_str(), force);

        std::string dup_hash = GetGlobalUUID(analyzer.c_str(), row.m_sDebugDumpDir.c_str());
        VERB3 log(" DUPHASH:'%s'", dup_hash.c_str());

        VERB3 log(" RunAnalyzerActions('%s','%s')", analyzer.c_str(), row.m_sDebugDumpDir.c_str());
        RunAnalyzerActions(analyzer.c_str(), row.m_sDebugDumpDir.c_str());

        DebugDumpToCrashReport(row.m_sDebugDumpDir.c_str(), pCrashData);

        add_to_crash_data_ext(pCrashData, CD_DUPHASH, CD_TXT, CD_ISNOTEDITABLE, dup_hash.c_str());
        add_to_crash_data_ext(pCrashData, CD_UUID   , CD_SYS, CD_ISNOTEDITABLE, pUUID);
    }
    catch (CABRTException& e)
    {
        r = MW_CORRUPTED;
        error_msg("%s", e.what());
        if (e.type() == EXCEP_DD_OPEN)
        {
            r = MW_ERROR;
        }
        else if (e.type() == EXCEP_DD_LOAD)
        {
            r = MW_FILE_ERROR;
        }
        else if (e.type() == EXCEP_PLUGIN)
        {
            r = MW_PLUGIN_ERROR;
        }
    }

    VERB3 log("CreateCrashReport() returns %d", r);
    return r;
}

void RunAction(const char *pActionDir,
                            const char *pPluginName,
                            const char *pPluginArgs)
{
    CAction* action = g_pPluginManager->GetAction(pPluginName);
    if (!action)
    {
        /* GetAction() already complained */
        return;
    }
    try
    {
        action->Run(pActionDir, pPluginArgs);
    }
    catch (CABRTException& e)
    {
        error_msg("Execution of '%s' was not successful: %s", pPluginName, e.what());
    }
}

void RunActionsAndReporters(const char *pDebugDumpDir)
{
    vector_pair_string_string_t::iterator it_ar = s_vectorActionsAndReporters.begin();
    map_plugin_settings_t plugin_settings;
    for (; it_ar != s_vectorActionsAndReporters.end(); it_ar++)
    {
        const char *plugin_name = it_ar->first.c_str();
        try
        {
            VERB3 log("RunActionsAndReporters: checking %s", plugin_name);
            plugin_type_t tp = g_pPluginManager->GetPluginType(plugin_name);
            if (tp == REPORTER)
            {
                CReporter* reporter = g_pPluginManager->GetReporter(plugin_name); /* can't be NULL */
                map_crash_data_t crashReport;
                DebugDumpToCrashReport(pDebugDumpDir, crashReport);
                VERB2 log("%s.Report(...)", plugin_name);
                reporter->Report(crashReport, plugin_settings, it_ar->second.c_str());
            }
            else if (tp == ACTION)
            {
                CAction* action = g_pPluginManager->GetAction(plugin_name); /* can't be NULL */
                VERB2 log("%s.Run('%s','%s')", plugin_name, pDebugDumpDir, it_ar->second.c_str());
                action->Run(pDebugDumpDir, it_ar->second.c_str());
            }
        }
        catch (CABRTException& e)
        {
            error_msg("Activation of plugin '%s' was not successful: %s", plugin_name, e.what());
        }
    }
}


// We must not trust client_report here!
// dbus handler passes it from user without checking
report_status_t Report(const map_crash_data_t& client_report,
                       map_map_string_t& pSettings,
                       const char *pUID)
{
    // Get ID fields
    const char *UID = get_crash_data_item_content_or_NULL(client_report, FILENAME_UID);
    const char *UUID = get_crash_data_item_content_or_NULL(client_report, CD_UUID);
    if (!UID || !UUID) {
        throw CABRTException(EXCEP_ERROR, "Report(): UID or UUID is missing in client's report data");
    }

    // Retrieve corresponding stored record
    map_crash_data_t stored_report;
    mw_result_t r = FillCrashInfo(UUID, UID, stored_report);
    if (r != MW_OK)
        return report_status_t();
    const std::string& pDumpDir = get_crash_data_item_content(stored_report, CD_DUMPDIR);

    // Save comment, "how to reproduce", backtrace
    const char *comment = get_crash_data_item_content_or_NULL(client_report, FILENAME_COMMENT);
    const char *reproduce = get_crash_data_item_content_or_NULL(client_report, FILENAME_REPRODUCE);
    const char *backtrace = get_crash_data_item_content_or_NULL(client_report, FILENAME_BACKTRACE);
    if (comment || reproduce || backtrace)
    {
        CDebugDump dd;
        dd.Open(pDumpDir.c_str());
        if (comment)
        {
            dd.SaveText(FILENAME_COMMENT, comment);
            add_to_crash_data_ext(stored_report, FILENAME_COMMENT, CD_TXT, CD_ISEDITABLE, comment);
        }
        if (reproduce)
        {
            dd.SaveText(FILENAME_REPRODUCE, reproduce);
            add_to_crash_data_ext(stored_report, FILENAME_REPRODUCE, CD_TXT, CD_ISEDITABLE, reproduce);
        }
        if (backtrace)
        {
            dd.SaveText(FILENAME_BACKTRACE, backtrace);
            add_to_crash_data_ext(stored_report, FILENAME_BACKTRACE, CD_TXT, CD_ISEDITABLE, backtrace);
        }
    }

    const std::string& analyzer = get_crash_data_item_content(stored_report, FILENAME_ANALYZER);

    std::string dup_hash = GetGlobalUUID(analyzer.c_str(), pDumpDir.c_str());
    VERB3 log(" DUPHASH:'%s'", dup_hash.c_str());
    add_to_crash_data_ext(stored_report, CD_DUPHASH, CD_TXT, CD_ISNOTEDITABLE, dup_hash.c_str());

    // Run reporters

    VERB3 {
        log("Run reporters");
        log_map_crash_data(client_report, " client_report");
        log_map_crash_data(stored_report, " stored_report");
    }
#define client_report client_report_must_not_be_used_below

    map_crash_data_t::const_iterator its_PACKAGE = stored_report.find(FILENAME_PACKAGE);
    std::string packageNVR = its_PACKAGE->second[CD_CONTENT];
    std::string packageName = packageNVR.substr(0, packageNVR.rfind("-", packageNVR.rfind("-") - 1));

    // analyzer with package name (CCpp:xorg-x11-app) has higher priority
    std::string key = analyzer + ":" + packageName;
    map_analyzer_actions_and_reporters_t::iterator end = s_mapAnalyzerActionsAndReporters.end();
    map_analyzer_actions_and_reporters_t::iterator keyPtr = s_mapAnalyzerActionsAndReporters.find(key);
    if (keyPtr == end)
    {
        // if there is no such settings, then try default analyzer
        keyPtr = s_mapAnalyzerActionsAndReporters.find(analyzer);
        key = analyzer;
    }

    report_status_t ret;
    std::string message;
    if (keyPtr != end)
    {
        VERB2 log("Found AnalyzerActionsAndReporters for '%s'", key.c_str());

        vector_pair_string_string_t::iterator it_r = keyPtr->second.begin();
        for (; it_r != keyPtr->second.end(); it_r++)
        {
            const char *plugin_name = it_r->first.c_str();
            try
            {
                if (g_pPluginManager->GetPluginType(plugin_name) == REPORTER)
                {
                    CReporter* reporter = g_pPluginManager->GetReporter(plugin_name); /* can't be NULL */
#if 0 /* Using ~user/.abrt/ is bad wrt security */
                    std::string home;
                    map_plugin_settings_t oldSettings;
                    map_plugin_settings_t newSettings;

                    if (pUID != "")
                    {
                        home = get_home_dir(xatoi_u(pUID.c_str()));
                        if (home != "")
                        {
                            oldSettings = reporter->GetSettings();

                            if (LoadPluginSettings(home + "/.abrt/" + plugin_name + "."PLUGINS_CONF_EXTENSION, newSettings))
                            {
                                reporter->SetSettings(newSettings);
                            }
                        }
                    }
#endif
                    map_plugin_settings_t plugin_settings = pSettings[plugin_name];
                    std::string res = reporter->Report(stored_report, plugin_settings, it_r->second.c_str());

#if 0 /* Using ~user/.abrt/ is bad wrt security */
                    if (home != "")
                    {
                        reporter->SetSettings(oldSettings);
                    }
#endif
                    ret[plugin_name].push_back("1"); // REPORT_STATUS_IDX_FLAG
                    ret[plugin_name].push_back(res); // REPORT_STATUS_IDX_MSG
                    if (message != "")
                        message += "; ";
                    message += res;
                }
            }
            catch (CABRTException& e)
            {
                ret[plugin_name].push_back("0");      // REPORT_STATUS_IDX_FLAG
                ret[plugin_name].push_back(e.what()); // REPORT_STATUS_IDX_MSG
                update_client("Reporting via '%s' was not successful: %s", plugin_name, e.what());
            }
        }
    }

    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
    database->Connect();
    database->SetReported(UUID, UID, message.c_str());
    database->DisConnect();

    return ret;
#undef client_report
}

/**
 * Check whether particular debugdump directory is saved
 * in database. This check is done together with an UID of an user.
 * @param pUID an UID of an user.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @return It returns true if debugdump dir is already saved, otherwise
 * it returns false.
 */
static bool IsDebugDumpSaved(const char *pUID,
                                   const char *pDebugDumpDir)
{
    /* TODO: use database query instead of dumping all rows and searching in them */

    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
    database->Connect();
    vector_database_rows_t rows = database->GetUIDData(pUID);
    database->DisConnect();

    int ii;
    bool found = false;
    for (ii = 0; ii < rows.size(); ii++)
    {
        if (rows[ii].m_sDebugDumpDir == pDebugDumpDir)
        {
            found = true;
            break;
        }
    }

    return found;
}

void LoadOpenGPGPublicKey(const char* key)
{
    VERB1 log("Loading GPG key '%s'", key);
    s_RPM.LoadOpenGPGPublicKey(key);
}

/**
 * Get a package name from executable name and save
 * package description to particular debugdump directory of a crash.
 * @param pExecutable A name of crashed application.
 * @param pDebugDumpDir A debugdump dir containing all necessary data.
 * @return It return results of operation. See mw_result_t.
 */
static char *get_argv1_if_full_path(const char* cmdline)
{
    char *argv1 = (char*) strchr(cmdline, ' ');
    if (argv1 != NULL)
    {
        /* we found space in cmdline, so it might contain
         * path to some script like:
         * /usr/bin/python /usr/bin/system-control-network
         */
        argv1++;
        /* if the string following the space doesn't start
         * with '/' it's probably not a full path to script
         * and we can't use it to determine the package name
         */
        if (*argv1 != '/')
        {
            return NULL;
        }
        int len = strchrnul(argv1, ' ') - argv1;
        /* cut the cmdline arguments */
        argv1 = xstrndup(argv1, len);
    }
    return argv1;
}
static mw_result_t SavePackageDescriptionToDebugDump(
                const char *pExecutable,
                const char *cmdline,
                const char *pDebugDumpDir)
{
    std::string package;
    std::string packageName;
    std::string scriptName; /* only if "interpreter /path/to/script" */

    if (strcmp(pExecutable, "kernel") == 0)
    {
        packageName = package = "kernel";
    }
    else
    {
        char *rpm_pkg = GetPackage(pExecutable);
        if (rpm_pkg == NULL)
        {
            log("Executable '%s' doesn't belong to any package", pExecutable);
            return MW_PACKAGE_ERROR;
        }

        /* Check well-known interpreter names */

        const char *basename = strrchr(pExecutable, '/');
        if (basename) basename++; else basename = pExecutable;

        /* Add "perl" and such as needed */
        if (strcmp(basename, "python") == 0)
        {
// TODO: we don't verify that python executable is not modified
// or that python package is properly signed
// (see CheckFingerprint/CheckHash below)

            /* Try to find package for the script by looking at argv[1].
             * This will work only of the cmdline contains the whole path.
             * Example: python /usr/bin/system-control-network
             */
            char *script_name = get_argv1_if_full_path(cmdline);
            if (script_name)
            {
                char *script_pkg = GetPackage(script_name);
                if (script_pkg)
                {
                    /* There is a well-formed script name in argv[1],
                     * and it does belong to some package.
                     * Replace interpreter's rpm_pkg and pExecutable
                     * with data pertaining to the script.
                     */
                    free(rpm_pkg);
                    rpm_pkg = script_pkg;
                    scriptName = script_name;
                    pExecutable = scriptName.c_str();
                }
                free(script_name);
            }
        }

        package = rpm_pkg;
        packageName = package.substr(0, package.rfind("-", package.rfind("-") - 1));
        VERB2 log("Package:'%s' short:'%s'", rpm_pkg, packageName.c_str());
        free(rpm_pkg);

	if (g_setBlackList.find(packageName) != g_setBlackList.end())
        {
            log("Blacklisted package '%s'", packageName.c_str());
            return MW_BLACKLISTED;
        }
        if (g_settings_bOpenGPGCheck)
        {
            if (!s_RPM.CheckFingerprint(packageName.c_str()))
            {
                log("Package '%s' isn't signed with proper key", packageName.c_str());
                return MW_GPG_ERROR;
            }
            if (!CheckHash(packageName.c_str(), pExecutable))
            {
                error_msg("Executable '%s' seems to be modified, "
                                "doesn't match one from package '%s'",
                                pExecutable, packageName.c_str());
                return MW_GPG_ERROR;
            }
        }
    }

    std::string description = GetDescription(packageName.c_str());
    std::string component = GetComponent(pExecutable);
    try
    {
        CDebugDump dd;
        dd.Open(pDebugDumpDir);
        dd.SaveText(FILENAME_PACKAGE, package.c_str());
        dd.SaveText(FILENAME_DESCRIPTION, description.c_str());
        dd.SaveText(FILENAME_COMPONENT, component.c_str());
    }
    catch (CABRTException& e)
    {
        error_msg("%s", e.what());
        if (e.type() == EXCEP_DD_SAVE)
        {
            return MW_FILE_ERROR;
        }
        return MW_ERROR;
    }

    return MW_OK;
}

bool analyzer_has_InformAllUsers(const char *analyzer_name)
{
    CAnalyzer* analyzer = g_pPluginManager->GetAnalyzer(analyzer_name);
    if (!analyzer)
    {
        return false;
    }
    map_plugin_settings_t settings = analyzer->GetSettings();
    map_plugin_settings_t::const_iterator it = settings.find("InformAllUsers");
    if (it == settings.end())
        return false;
    return string_to_bool(it->second.c_str());
}

bool analyzer_has_AutoReportUIDs(const char *analyzer_name, const char *uid_str)
{
    CAnalyzer* analyzer = g_pPluginManager->GetAnalyzer(analyzer_name);
    if (!analyzer)
    {
        return false;
    }
    map_plugin_settings_t settings = analyzer->GetSettings();
    map_plugin_settings_t::const_iterator it = settings.find("AutoReportUIDs");
    if (it == settings.end())
        return false;

    if ((strcmp(analyzer_name, "Kerneloops") == 0) && (strcmp(uid_str, "-1") == 0))
        return true;

/*
    vector_string_t logins;
    parse_args(it->second.c_str(), logins);

    uid_t uid = xatoi_u(uid_str);
    unsigned size = logins.size();
    for (unsigned ii = 0; ii < size; ii++)
    {
        struct passwd* pw = getpwnam(logins[ii].c_str());
        if (!pw)
            continue;
        if (pw->pw_uid == uid)
            return true;
    }
*/
    return false;
}

void autoreport(const pair_string_string_t& reporter_options, const map_crash_data_t& crash_report)
{
    CReporter* reporter = g_pPluginManager->GetReporter(reporter_options.first.c_str());
    if (!reporter)
    {
        return;
    }
    map_plugin_settings_t plugin_settings;
    /*std::string res =*/ reporter->Report(crash_report, plugin_settings, reporter_options.second.c_str());
}

/**
 * Execute all action plugins, which are associated to
 * particular analyzer plugin.
 * @param pAnalyzer A name of an analyzer plugin.
 * @param pDebugDumpPath A debugdump dir containing all necessary data.
 */
static void RunAnalyzerActions(const char *pAnalyzer, const char *pDebugDumpDir)
{
    map_analyzer_actions_and_reporters_t::iterator analyzer = s_mapAnalyzerActionsAndReporters.find(pAnalyzer);
    if (analyzer != s_mapAnalyzerActionsAndReporters.end())
    {
        vector_pair_string_string_t::iterator it_a = analyzer->second.begin();
        for (; it_a != analyzer->second.end(); it_a++)
        {
            const char *plugin_name = it_a->first.c_str();
            CAction* action = g_pPluginManager->GetAction(plugin_name, /*silent:*/ true);
            if (!action)
            {
                /* GetAction() already complained if no such plugin.
		 * If plugin exists but isn't an Action, it's not an error.
		 */
                continue;
            }
            try
            {
                action->Run(pDebugDumpDir, it_a->second.c_str());
            }
            catch (CABRTException& e)
            {
                update_client("Action performed by '%s' was not successful: %s", plugin_name, e.what());
            }
        }
    }
}

/**
 * Save a debugdump into database. If saving is
 * successful, then crash info is filled. Otherwise the crash info is
 * not changed.
 * @param pUUID A local UUID of a crash.
 * @param pUID An UID of an user.
 * @param pTime Time when a crash occurs.
 * @param pDebugDumpPath A debugdump path.
 * @param pCrashData A filled crash info.
 * @return It return results of operation. See mw_result_t.
 */
static mw_result_t SaveDebugDumpToDatabase(const char *pUUID,
                const char *pUID,
                const char *pTime,
                const char *pDebugDumpDir,
                map_crash_data_t& pCrashData)
{
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
    database->Connect();
    /* note: if [UUID,UID] record exists, pDebugDumpDir is not updated in the record */
    database->Insert_or_Update(pUUID, pUID, pDebugDumpDir, pTime);
    database_row_t row = database->GetRow(pUUID, pUID);
    database->DisConnect();

    mw_result_t res = FillCrashInfo(pUUID, pUID, pCrashData);
    if (res == MW_OK)
    {
        if (row.m_sReported == "1")
        {
            log("Crash is already reported");
            return MW_REPORTED;
        }
        if (row.m_sCount != "1")
        {
            log("Crash is in database already");
            return MW_OCCURED;
        }
    }
    return res;
}

std::string getDebugDumpDir(const char *pUUID, const char *pUID)
{
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
    database->Connect();
    database_row_t row = database->GetRow(pUUID, pUID);
    database->DisConnect();
    return row.m_sDebugDumpDir;
}

mw_result_t SaveDebugDump(const char *pDebugDumpDir,
                map_crash_data_t& pCrashData)
{
    std::string UID;
    std::string time;
    std::string analyzer;
    std::string executable;
    std::string cmdline;
    try
    {
        CDebugDump dd;
        dd.Open(pDebugDumpDir);
        dd.LoadText(FILENAME_TIME, time);
        dd.LoadText(FILENAME_UID, UID);
        dd.LoadText(FILENAME_ANALYZER, analyzer);
        dd.LoadText(FILENAME_EXECUTABLE, executable);
        dd.LoadText(FILENAME_CMDLINE, cmdline);
    }
    catch (CABRTException& e)
    {
        error_msg("%s", e.what());
        if (e.type() == EXCEP_DD_SAVE)
        {
            return MW_FILE_ERROR;
        }
        return MW_ERROR;
    }

    if (IsDebugDumpSaved(UID.c_str(), pDebugDumpDir))
    {
        return MW_IN_DB;
    }

    mw_result_t res = SavePackageDescriptionToDebugDump(executable.c_str(), cmdline.c_str(), pDebugDumpDir);
    if (res != MW_OK)
    {
        return res;
    }

    std::string lUUID = GetLocalUUID(analyzer.c_str(), pDebugDumpDir);
    const char *uid_str = analyzer_has_InformAllUsers(analyzer.c_str())
        ? "-1"
        : UID.c_str();
    return SaveDebugDumpToDatabase(lUUID.c_str(), uid_str, time.c_str(), pDebugDumpDir, pCrashData);
}

mw_result_t FillCrashInfo(const char *pUUID,
                const char *pUID,
                map_crash_data_t& pCrashData)
{
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
    database->Connect();
    database_row_t row = database->GetRow(pUUID, pUID);
    database->DisConnect();

    std::string package;
    std::string executable;
    std::string description;
    std::string analyzer;
    try
    {
        CDebugDump dd;
        dd.Open(row.m_sDebugDumpDir.c_str());
        load_crash_data_from_debug_dump(dd, pCrashData);
    }
    catch (CABRTException& e)
    {
        error_msg("%s", e.what());
        return MW_ERROR;
    }

    add_to_crash_data(pCrashData, CD_UUID             , row.m_sUUID.c_str()        );
    add_to_crash_data(pCrashData, CD_COUNT            , row.m_sCount.c_str()       );
    add_to_crash_data(pCrashData, CD_REPORTED         , row.m_sReported.c_str()    );
    add_to_crash_data(pCrashData, CD_MESSAGE          , row.m_sMessage.c_str()     );
    add_to_crash_data(pCrashData, CD_DUMPDIR          , row.m_sDebugDumpDir.c_str());
//TODO: why do we keep uid and time in DB and in dumpdir?!
    add_to_crash_data(pCrashData, FILENAME_UID        , row.m_sUID.c_str()         );
    add_to_crash_data(pCrashData, FILENAME_TIME       , row.m_sTime.c_str()        );

    return MW_OK;
}

vector_pair_string_string_t GetUUIDsOfCrash(const char *pUID)
{
    CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
    vector_database_rows_t rows;
    database->Connect();
    rows = database->GetUIDData(pUID);
    database->DisConnect();

    vector_pair_string_string_t UUIDsUIDs;
    unsigned ii;
    for (ii = 0; ii < rows.size(); ii++)
    {
        UUIDsUIDs.push_back(make_pair(rows[ii].m_sUUID, rows[ii].m_sUID));
    }

    return UUIDsUIDs;
}

void AddAnalyzerActionOrReporter(const char *pAnalyzer,
                                              const char *pAnalyzerOrReporter,
                                              const char *pArgs)
{
    s_mapAnalyzerActionsAndReporters[pAnalyzer].push_back(make_pair(std::string(pAnalyzerOrReporter), std::string(pArgs)));
}

void AddActionOrReporter(const char *pActionOrReporter,
                                      const char *pArgs)
{
    VERB3 log("AddActionOrReporter('%s','%s')", pActionOrReporter, pArgs);
    s_vectorActionsAndReporters.push_back(make_pair(std::string(pActionOrReporter), std::string(pArgs)));
}
