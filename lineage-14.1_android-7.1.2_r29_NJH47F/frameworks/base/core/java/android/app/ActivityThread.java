/*
 * Copyright (C) 2006 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.app;

import android.annotation.NonNull;
import android.annotation.Nullable;
import android.app.assist.AssistContent;
import android.app.assist.AssistStructure;
import android.app.backup.BackupAgent;
import android.content.BroadcastReceiver;
import android.content.ComponentCallbacks2;
import android.content.ComponentName;
import android.content.ContentProvider;
import android.content.Context;
import android.content.IContentProvider;
import android.content.Intent;
import android.content.IIntentReceiver;
import android.content.pm.ActivityInfo;
import android.content.pm.ApplicationInfo;
import android.content.pm.IPackageManager;
import android.content.pm.InstrumentationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.PackageManager.NameNotFoundException;
import android.content.pm.ProviderInfo;
import android.content.pm.ServiceInfo;
import android.content.res.AssetManager;
import android.content.res.CompatibilityInfo;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.content.res.Resources.Theme;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteDebug;
import android.database.sqlite.SQLiteDebug.DbStats;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.hardware.display.DisplayManagerGlobal;
import android.net.ConnectivityManager;
import android.net.IConnectivityManager;
import android.net.Network;
import android.net.Proxy;
import android.net.ProxyInfo;
import android.net.Uri;
import android.os.AsyncTask;
import android.os.Binder;
import android.os.Build;
import android.os.Bundle;
import android.os.Debug;
import android.os.DropBoxManager;
import android.os.Environment;
import android.os.GraphicsEnvironment;
import android.os.Handler;
import android.os.IBinder;
import android.os.LocaleList;
import android.os.Looper;
import android.os.Message;
import android.os.MessageQueue;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.os.PersistableBundle;
import android.os.Process;
import android.os.RemoteException;
import android.os.ServiceManager;
import android.os.StrictMode;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.os.Trace;
import android.os.TransactionTooLargeException;
import android.os.UserHandle;
import android.provider.Settings;
import android.security.NetworkSecurityPolicy;
import android.security.net.config.NetworkSecurityConfigProvider;
import android.util.AndroidRuntimeException;
import android.util.ArrayMap;
import android.util.DisplayMetrics;
import android.util.EventLog;
import android.util.Log;
import android.util.LogPrinter;
import android.util.Pair;
import android.util.PrintWriterPrinter;
import android.util.Slog;
import android.util.SparseIntArray;
import android.util.SuperNotCalledException;
import android.view.ContextThemeWrapper;
import android.view.Display;
import android.view.ThreadedRenderer;
import android.view.View;
import android.view.ViewDebug;
import android.view.ViewManager;
import android.view.ViewRootImpl;
import android.view.Window;
import android.view.WindowManager;
import android.view.WindowManagerGlobal;
import android.renderscript.RenderScriptCacheDir;
import android.system.Os;
import android.system.OsConstants;
import android.system.ErrnoException;
import android.webkit.WebView;

import com.android.internal.annotations.GuardedBy;
import com.android.internal.app.IVoiceInteractor;
import com.android.internal.content.ReferrerIntent;
import com.android.internal.os.BinderInternal;
import com.android.internal.os.RuntimeInit;
import com.android.internal.os.SamplingProfilerIntegration;
import com.android.internal.os.SomeArgs;
import com.android.internal.policy.DecorView;
import com.android.internal.util.ArrayUtils;
import com.android.internal.util.FastPrintWriter;
import com.android.org.conscrypt.OpenSSLSocketImpl;
import com.android.org.conscrypt.TrustedCertificateStore;
import com.google.android.collect.Lists;

import java.io.File;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.PrintWriter;
import java.lang.ref.WeakReference;
import java.net.InetAddress;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Objects;
import java.util.TimeZone;

import libcore.io.DropBox;
import libcore.io.EventLogger;
import libcore.io.IoUtils;
import libcore.net.event.NetworkEventDispatcher;
import dalvik.system.CloseGuard;
import dalvik.system.VMDebug;
import dalvik.system.VMRuntime;
import org.apache.harmony.dalvik.ddmc.DdmVmInternal;

//patch by Youlor
//++++++++++++++++++++++++++++
import cn.youlor.Unpacker;
//++++++++++++++++++++++++++++

final class RemoteServiceException extends AndroidRuntimeException {
    public RemoteServiceException(String msg) {
        super(msg);
    }
}

/**
 * This manages the execution of the main thread in an
 * application process, scheduling and executing activities,
 * broadcasts, and other operations on it as the activity
 * manager requests.
 *
 * {@hide}
 */
public final class ActivityThread {
    /** @hide */
    public static final String TAG = "ActivityThread";
    private static final android.graphics.Bitmap.Config THUMBNAIL_FORMAT = Bitmap.Config.RGB_565;
    static final boolean localLOGV = false;
    static final boolean DEBUG_MESSAGES = false;
    /** @hide */
    public static final boolean DEBUG_BROADCAST = false;
    private static final boolean DEBUG_RESULTS = false;
    private static final boolean DEBUG_BACKUP = false;
    public static final boolean DEBUG_CONFIGURATION = false;
    private static final boolean DEBUG_SERVICE = false;
    private static final boolean DEBUG_MEMORY_TRIM = false;
    private static final boolean DEBUG_PROVIDER = false;
    private static final boolean DEBUG_ORDER = false;
    private static final long MIN_TIME_BETWEEN_GCS = 5*1000;
    private static final int SQLITE_MEM_RELEASED_EVENT_LOG_TAG = 75003;
    private static final int LOG_AM_ON_PAUSE_CALLED = 30021;
    private static final int LOG_AM_ON_RESUME_CALLED = 30022;
    private static final int LOG_AM_ON_STOP_CALLED = 30049;

    /** Type for IActivityManager.serviceDoneExecuting: anonymous operation */
    public static final int SERVICE_DONE_EXECUTING_ANON = 0;
    /** Type for IActivityManager.serviceDoneExecuting: done with an onStart call */
    public static final int SERVICE_DONE_EXECUTING_START = 1;
    /** Type for IActivityManager.serviceDoneExecuting: done stopping (destroying) service */
    public static final int SERVICE_DONE_EXECUTING_STOP = 2;

    // Details for pausing activity.
    private static final int USER_LEAVING = 1;
    private static final int DONT_REPORT = 2;

    // Whether to invoke an activity callback after delivering new configuration.
    private static final boolean REPORT_TO_ACTIVITY = true;

    private ContextImpl mSystemContext;

    static volatile IPackageManager sPackageManager;

    final ApplicationThread mAppThread = new ApplicationThread();
    final Looper mLooper = Looper.myLooper();
    final H mH = new H();
    final ArrayMap<IBinder, ActivityClientRecord> mActivities = new ArrayMap<>();
    // List of new activities (via ActivityRecord.nextIdle) that should
    // be reported when next we idle.
    ActivityClientRecord mNewActivities = null;
    // Number of activities that are currently visible on-screen.
    int mNumVisibleActivities = 0;
    ArrayList<WeakReference<AssistStructure>> mLastAssistStructures = new ArrayList<>();
    private int mLastSessionId;
    final ArrayMap<IBinder, Service> mServices = new ArrayMap<>();
    AppBindData mBoundApplication;
    Profiler mProfiler;
    int mCurDefaultDisplayDpi;
    boolean mDensityCompatMode;
    Configuration mConfiguration;
    Configuration mCompatConfiguration;
    Application mInitialApplication;
    final ArrayList<Application> mAllApplications
            = new ArrayList<Application>();
    // set of instantiated backup agents, keyed by package name
    final ArrayMap<String, BackupAgent> mBackupAgents = new ArrayMap<String, BackupAgent>();
    /** Reference to singleton {@link ActivityThread} */
    private static volatile ActivityThread sCurrentActivityThread;
    Instrumentation mInstrumentation;
    String mInstrumentationPackageName = null;
    String mInstrumentationAppDir = null;
    String[] mInstrumentationSplitAppDirs = null;
    String mInstrumentationLibDir = null;
    String mInstrumentedAppDir = null;
    String[] mInstrumentedSplitAppDirs = null;
    String mInstrumentedLibDir = null;
    boolean mSystemThread = false;
    boolean mJitEnabled = false;
    boolean mSomeActivitiesChanged = false;
    boolean mUpdatingSystemConfig = false;

    // These can be accessed by multiple threads; mPackages is the lock.
    // XXX For now we keep around information about all packages we have
    // seen, not removing entries from this map.
    // NOTE: The activity and window managers need to call in to
    // ActivityThread to do things like update resource configurations,
    // which means this lock gets held while the activity and window managers
    // holds their own lock.  Thus you MUST NEVER call back into the activity manager
    // or window manager or anything that depends on them while holding this lock.
    // These LoadedApk are only valid for the userId that we're running as.
    final ArrayMap<String, WeakReference<LoadedApk>> mPackages
            = new ArrayMap<String, WeakReference<LoadedApk>>();
    final ArrayMap<String, WeakReference<LoadedApk>> mResourcePackages
            = new ArrayMap<String, WeakReference<LoadedApk>>();
    final ArrayList<ActivityClientRecord> mRelaunchingActivities
            = new ArrayList<ActivityClientRecord>();
    Configuration mPendingConfiguration = null;
    // Because we merge activity relaunch operations we can't depend on the ordering provided by
    // the handler messages. We need to introduce secondary ordering mechanism, which will allow
    // us to drop certain events, if we know that they happened before relaunch we already executed.
    // This represents the order of receiving the request from AM.
    @GuardedBy("mResourcesManager")
    int mLifecycleSeq = 0;

    private final ResourcesManager mResourcesManager;

    private static final class ProviderKey {
        final String authority;
        final int userId;

        public ProviderKey(String authority, int userId) {
            this.authority = authority;
            this.userId = userId;
        }

        @Override
        public boolean equals(Object o) {
            if (o instanceof ProviderKey) {
                final ProviderKey other = (ProviderKey) o;
                return Objects.equals(authority, other.authority) && userId == other.userId;
            }
            return false;
        }

        @Override
        public int hashCode() {
            return ((authority != null) ? authority.hashCode() : 0) ^ userId;
        }
    }

    // The lock of mProviderMap protects the following variables.
    final ArrayMap<ProviderKey, ProviderClientRecord> mProviderMap
        = new ArrayMap<ProviderKey, ProviderClientRecord>();
    final ArrayMap<ProviderKey, ProviderAcquiringCount> mProviderAcquiringCountMap
        = new ArrayMap<>();
    final ArrayMap<IBinder, ProviderRefCount> mProviderRefCountMap
        = new ArrayMap<IBinder, ProviderRefCount>();
    final ArrayMap<IBinder, ProviderClientRecord> mLocalProviders
        = new ArrayMap<IBinder, ProviderClientRecord>();
    final ArrayMap<ComponentName, ProviderClientRecord> mLocalProvidersByName
            = new ArrayMap<ComponentName, ProviderClientRecord>();

    final ArrayMap<Activity, ArrayList<OnActivityPausedListener>> mOnPauseListeners
        = new ArrayMap<Activity, ArrayList<OnActivityPausedListener>>();

    final GcIdler mGcIdler = new GcIdler();
    boolean mGcIdlerScheduled = false;

    static volatile Handler sMainThreadHandler;  // set once in main()

    Bundle mCoreSettings = null;

    static final class ActivityClientRecord {
        IBinder token;
        int ident;
        Intent intent;
        String referrer;
        IVoiceInteractor voiceInteractor;
        Bundle state;
        PersistableBundle persistentState;
        Activity activity;
        Window window;
        Activity parent;
        String embeddedID;
        Activity.NonConfigurationInstances lastNonConfigurationInstances;
        boolean paused;
        boolean stopped;
        boolean hideForNow;
        Configuration newConfig;
        Configuration createdConfig;
        Configuration overrideConfig;
        // Used for consolidating configs before sending on to Activity.
        private Configuration tmpConfig = new Configuration();
        ActivityClientRecord nextIdle;

        ProfilerInfo profilerInfo;

        ActivityInfo activityInfo;
        CompatibilityInfo compatInfo;
        LoadedApk packageInfo;

        List<ResultInfo> pendingResults;
        List<ReferrerIntent> pendingIntents;

        boolean startsNotResumed;
        boolean isForward;
        int pendingConfigChanges;
        boolean onlyLocalRequest;

        Window mPendingRemoveWindow;
        WindowManager mPendingRemoveWindowManager;
        boolean mPreserveWindow;

        // Set for relaunch requests, indicates the order number of the relaunch operation, so it
        // can be compared with other lifecycle operations.
        int relaunchSeq = 0;

        // Can only be accessed from the UI thread. This represents the latest processed message
        // that is related to lifecycle events/
        int lastProcessedSeq = 0;

        ActivityClientRecord() {
            parent = null;
            embeddedID = null;
            paused = false;
            stopped = false;
            hideForNow = false;
            nextIdle = null;
        }

        public boolean isPreHoneycomb() {
            if (activity != null) {
                return activity.getApplicationInfo().targetSdkVersion
                        < android.os.Build.VERSION_CODES.HONEYCOMB;
            }
            return false;
        }

        public boolean isPersistable() {
            return activityInfo.persistableMode == ActivityInfo.PERSIST_ACROSS_REBOOTS;
        }

        public boolean isInStack() {
            try {
                int stackId = ActivityManagerNative.getDefault().getActivityStackId(token);
                int taskId = ActivityManagerNative.getDefault().getTaskForActivity(token, false);
                // INVALID_STACK_ID = -1 and INVALID_TASK_ID = -1
                if (stackId != -1 && taskId != -1) {
                    return true;
                }
            } catch (RemoteException e) {
                Log.w(TAG, "remote exception occur while check the task and stack of activity:"
                        + this.toString(), e);
            }
            return false;
        }

        public String toString() {
            ComponentName componentName = intent != null ? intent.getComponent() : null;
            return "ActivityRecord{"
                + Integer.toHexString(System.identityHashCode(this))
                + " token=" + token + " " + (componentName == null
                        ? "no component name" : componentName.toShortString())
                + "}";
        }

        public String getStateString() {
            StringBuilder sb = new StringBuilder();
            sb.append("ActivityClientRecord{");
            sb.append("paused=").append(paused);
            sb.append(", stopped=").append(stopped);
            sb.append(", hideForNow=").append(hideForNow);
            sb.append(", startsNotResumed=").append(startsNotResumed);
            sb.append(", isForward=").append(isForward);
            sb.append(", pendingConfigChanges=").append(pendingConfigChanges);
            sb.append(", onlyLocalRequest=").append(onlyLocalRequest);
            sb.append(", preserveWindow=").append(mPreserveWindow);
            if (activity != null) {
                sb.append(", Activity{");
                sb.append("resumed=").append(activity.mResumed);
                sb.append(", stopped=").append(activity.mStopped);
                sb.append(", finished=").append(activity.isFinishing());
                sb.append(", destroyed=").append(activity.isDestroyed());
                sb.append(", startedActivity=").append(activity.mStartedActivity);
                sb.append(", temporaryPause=").append(activity.mTemporaryPause);
                sb.append(", changingConfigurations=").append(activity.mChangingConfigurations);
                sb.append(", visibleBehind=").append(activity.mVisibleBehind);
                sb.append("}");
            }
            sb.append("}");
            return sb.toString();
        }
    }

    final class ProviderClientRecord {
        final String[] mNames;
        final IContentProvider mProvider;
        final ContentProvider mLocalProvider;
        final IActivityManager.ContentProviderHolder mHolder;

        ProviderClientRecord(String[] names, IContentProvider provider,
                ContentProvider localProvider,
                IActivityManager.ContentProviderHolder holder) {
            mNames = names;
            mProvider = provider;
            mLocalProvider = localProvider;
            mHolder = holder;
        }
    }

    static final class NewIntentData {
        List<ReferrerIntent> intents;
        IBinder token;
        boolean andPause;
        public String toString() {
            return "NewIntentData{intents=" + intents + " token=" + token
                    + " andPause=" + andPause +"}";
        }
    }

    static final class ReceiverData extends BroadcastReceiver.PendingResult {
        public ReceiverData(Intent intent, int resultCode, String resultData, Bundle resultExtras,
                boolean ordered, boolean sticky, IBinder token, int sendingUser) {
            super(resultCode, resultData, resultExtras, TYPE_COMPONENT, ordered, sticky,
                    token, sendingUser, intent.getFlags());
            this.intent = intent;
        }

        Intent intent;
        ActivityInfo info;
        CompatibilityInfo compatInfo;
        public String toString() {
            return "ReceiverData{intent=" + intent + " packageName=" +
                    info.packageName + " resultCode=" + getResultCode()
                    + " resultData=" + getResultData() + " resultExtras="
                    + getResultExtras(false) + "}";
        }
    }

    static final class CreateBackupAgentData {
        ApplicationInfo appInfo;
        CompatibilityInfo compatInfo;
        int backupMode;
        public String toString() {
            return "CreateBackupAgentData{appInfo=" + appInfo
                    + " backupAgent=" + appInfo.backupAgentName
                    + " mode=" + backupMode + "}";
        }
    }

    static final class CreateServiceData {
        IBinder token;
        ServiceInfo info;
        CompatibilityInfo compatInfo;
        Intent intent;
        public String toString() {
            return "CreateServiceData{token=" + token + " className="
            + info.name + " packageName=" + info.packageName
            + " intent=" + intent + "}";
        }
    }

    static final class BindServiceData {
        IBinder token;
        Intent intent;
        boolean rebind;
        public String toString() {
            return "BindServiceData{token=" + token + " intent=" + intent + "}";
        }
    }

    static final class ServiceArgsData {
        IBinder token;
        boolean taskRemoved;
        int startId;
        int flags;
        Intent args;
        public String toString() {
            return "ServiceArgsData{token=" + token + " startId=" + startId
            + " args=" + args + "}";
        }
    }

    static final class AppBindData {
        LoadedApk info;
        String processName;
        ApplicationInfo appInfo;
        List<ProviderInfo> providers;
        ComponentName instrumentationName;
        Bundle instrumentationArgs;
        IInstrumentationWatcher instrumentationWatcher;
        IUiAutomationConnection instrumentationUiAutomationConnection;
        int debugMode;
        boolean enableBinderTracking;
        boolean trackAllocation;
        boolean restrictedBackupMode;
        boolean persistent;
        Configuration config;
        CompatibilityInfo compatInfo;

        /** Initial values for {@link Profiler}. */
        ProfilerInfo initProfilerInfo;

        public String toString() {
            return "AppBindData{appInfo=" + appInfo + "}";
        }
    }

    static final class Profiler {
        String profileFile;
        ParcelFileDescriptor profileFd;
        int samplingInterval;
        boolean autoStopProfiler;
        boolean profiling;
        boolean handlingProfiling;
        public void setProfiler(ProfilerInfo profilerInfo) {
            ParcelFileDescriptor fd = profilerInfo.profileFd;
            if (profiling) {
                if (fd != null) {
                    try {
                        fd.close();
                    } catch (IOException e) {
                        // Ignore
                    }
                }
                return;
            }
            if (profileFd != null) {
                try {
                    profileFd.close();
                } catch (IOException e) {
                    // Ignore
                }
            }
            profileFile = profilerInfo.profileFile;
            profileFd = fd;
            samplingInterval = profilerInfo.samplingInterval;
            autoStopProfiler = profilerInfo.autoStopProfiler;
        }
        public void startProfiling() {
            if (profileFd == null || profiling) {
                return;
            }
            try {
                int bufferSize = SystemProperties.getInt("debug.traceview-buffer-size-mb", 8);
                VMDebug.startMethodTracing(profileFile, profileFd.getFileDescriptor(),
                        bufferSize * 1024 * 1024, 0, samplingInterval != 0, samplingInterval);
                profiling = true;
            } catch (RuntimeException e) {
                Slog.w(TAG, "Profiling failed on path " + profileFile);
                try {
                    profileFd.close();
                    profileFd = null;
                } catch (IOException e2) {
                    Slog.w(TAG, "Failure closing profile fd", e2);
                }
            }
        }
        public void stopProfiling() {
            if (profiling) {
                profiling = false;
                Debug.stopMethodTracing();
                if (profileFd != null) {
                    try {
                        profileFd.close();
                    } catch (IOException e) {
                    }
                }
                profileFd = null;
                profileFile = null;
            }
        }
    }

    static final class DumpComponentInfo {
        ParcelFileDescriptor fd;
        IBinder token;
        String prefix;
        String[] args;
    }

    static final class ResultData {
        IBinder token;
        List<ResultInfo> results;
        public String toString() {
            return "ResultData{token=" + token + " results" + results + "}";
        }
    }

    static final class ContextCleanupInfo {
        ContextImpl context;
        String what;
        String who;
    }

    static final class DumpHeapData {
        String path;
        ParcelFileDescriptor fd;
    }

    static final class UpdateCompatibilityData {
        String pkg;
        CompatibilityInfo info;
    }

    static final class RequestAssistContextExtras {
        IBinder activityToken;
        IBinder requestToken;
        int requestType;
        int sessionId;
    }

    static final class ActivityConfigChangeData {
        final IBinder activityToken;
        final Configuration overrideConfig;
        public ActivityConfigChangeData(IBinder token, Configuration config) {
            activityToken = token;
            overrideConfig = config;
        }
    }

    private native void dumpGraphicsInfo(FileDescriptor fd);

    private class ApplicationThread extends ApplicationThreadNative {
        private static final String DB_INFO_FORMAT = "  %8s %8s %14s %14s  %s";

        private int mLastProcessState = -1;

        private void updatePendingConfiguration(Configuration config) {
            synchronized (mResourcesManager) {
                if (mPendingConfiguration == null ||
                        mPendingConfiguration.isOtherSeqNewer(config)) {
                    mPendingConfiguration = config;
                }
            }
        }

        public final void schedulePauseActivity(IBinder token, boolean finished,
                boolean userLeaving, int configChanges, boolean dontReport) {
            int seq = getLifecycleSeq();
            if (DEBUG_ORDER) Slog.d(TAG, "pauseActivity " + ActivityThread.this
                    + " operation received seq: " + seq);
            sendMessage(
                    finished ? H.PAUSE_ACTIVITY_FINISHING : H.PAUSE_ACTIVITY,
                    token,
                    (userLeaving ? USER_LEAVING : 0) | (dontReport ? DONT_REPORT : 0),
                    configChanges,
                    seq);
        }

        public final void scheduleStopActivity(IBinder token, boolean showWindow,
                int configChanges) {
            int seq = getLifecycleSeq();
            if (DEBUG_ORDER) Slog.d(TAG, "stopActivity " + ActivityThread.this
                    + " operation received seq: " + seq);
            sendMessage(
                showWindow ? H.STOP_ACTIVITY_SHOW : H.STOP_ACTIVITY_HIDE,
                token, 0, configChanges, seq);
        }

        public final void scheduleWindowVisibility(IBinder token, boolean showWindow) {
            sendMessage(
                showWindow ? H.SHOW_WINDOW : H.HIDE_WINDOW,
                token);
        }

        public final void scheduleSleeping(IBinder token, boolean sleeping) {
            sendMessage(H.SLEEPING, token, sleeping ? 1 : 0);
        }

        public final void scheduleResumeActivity(IBinder token, int processState,
                boolean isForward, Bundle resumeArgs) {
            int seq = getLifecycleSeq();
            if (DEBUG_ORDER) Slog.d(TAG, "resumeActivity " + ActivityThread.this
                    + " operation received seq: " + seq);
            updateProcessState(processState, false);
            sendMessage(H.RESUME_ACTIVITY, token, isForward ? 1 : 0, 0, seq);
        }

        public final void scheduleSendResult(IBinder token, List<ResultInfo> results) {
            ResultData res = new ResultData();
            res.token = token;
            res.results = results;
            sendMessage(H.SEND_RESULT, res);
        }

        // we use token to identify this activity without having to send the
        // activity itself back to the activity manager. (matters more with ipc)
        @Override
        public final void scheduleLaunchActivity(Intent intent, IBinder token, int ident,
                ActivityInfo info, Configuration curConfig, Configuration overrideConfig,
                CompatibilityInfo compatInfo, String referrer, IVoiceInteractor voiceInteractor,
                int procState, Bundle state, PersistableBundle persistentState,
                List<ResultInfo> pendingResults, List<ReferrerIntent> pendingNewIntents,
                boolean notResumed, boolean isForward, ProfilerInfo profilerInfo) {

            updateProcessState(procState, false);

            ActivityClientRecord r = new ActivityClientRecord();

            r.token = token;
            r.ident = ident;
            r.intent = intent;
            r.referrer = referrer;
            r.voiceInteractor = voiceInteractor;
            r.activityInfo = info;
            r.compatInfo = compatInfo;
            r.state = state;
            r.persistentState = persistentState;

            r.pendingResults = pendingResults;
            r.pendingIntents = pendingNewIntents;

            r.startsNotResumed = notResumed;
            r.isForward = isForward;

            r.profilerInfo = profilerInfo;

            r.overrideConfig = overrideConfig;
            updatePendingConfiguration(curConfig);

            sendMessage(H.LAUNCH_ACTIVITY, r);
        }

        @Override
        public final void scheduleRelaunchActivity(IBinder token,
                List<ResultInfo> pendingResults, List<ReferrerIntent> pendingNewIntents,
                int configChanges, boolean notResumed, Configuration config,
                Configuration overrideConfig, boolean preserveWindow) {
            requestRelaunchActivity(token, pendingResults, pendingNewIntents,
                    configChanges, notResumed, config, overrideConfig, true, preserveWindow);
        }

        public final void scheduleNewIntent(
                List<ReferrerIntent> intents, IBinder token, boolean andPause) {
            NewIntentData data = new NewIntentData();
            data.intents = intents;
            data.token = token;
            data.andPause = andPause;

            sendMessage(H.NEW_INTENT, data);
        }

        public final void scheduleDestroyActivity(IBinder token, boolean finishing,
                int configChanges) {
            sendMessage(H.DESTROY_ACTIVITY, token, finishing ? 1 : 0,
                    configChanges);
        }

        public final void scheduleReceiver(Intent intent, ActivityInfo info,
                CompatibilityInfo compatInfo, int resultCode, String data, Bundle extras,
                boolean sync, int sendingUser, int processState) {
            updateProcessState(processState, false);
            ReceiverData r = new ReceiverData(intent, resultCode, data, extras,
                    sync, false, mAppThread.asBinder(), sendingUser);
            r.info = info;
            r.compatInfo = compatInfo;
            sendMessage(H.RECEIVER, r);
        }

        public final void scheduleCreateBackupAgent(ApplicationInfo app,
                CompatibilityInfo compatInfo, int backupMode) {
            CreateBackupAgentData d = new CreateBackupAgentData();
            d.appInfo = app;
            d.compatInfo = compatInfo;
            d.backupMode = backupMode;

            sendMessage(H.CREATE_BACKUP_AGENT, d);
        }

        public final void scheduleDestroyBackupAgent(ApplicationInfo app,
                CompatibilityInfo compatInfo) {
            CreateBackupAgentData d = new CreateBackupAgentData();
            d.appInfo = app;
            d.compatInfo = compatInfo;

            sendMessage(H.DESTROY_BACKUP_AGENT, d);
        }

        public final void scheduleCreateService(IBinder token,
                ServiceInfo info, CompatibilityInfo compatInfo, int processState) {
            updateProcessState(processState, false);
            CreateServiceData s = new CreateServiceData();
            s.token = token;
            s.info = info;
            s.compatInfo = compatInfo;

            sendMessage(H.CREATE_SERVICE, s);
        }

        public final void scheduleBindService(IBinder token, Intent intent,
                boolean rebind, int processState) {
            updateProcessState(processState, false);
            BindServiceData s = new BindServiceData();
            s.token = token;
            s.intent = intent;
            s.rebind = rebind;

            if (DEBUG_SERVICE)
                Slog.v(TAG, "scheduleBindService token=" + token + " intent=" + intent + " uid="
                        + Binder.getCallingUid() + " pid=" + Binder.getCallingPid());
            sendMessage(H.BIND_SERVICE, s);
        }

        public final void scheduleUnbindService(IBinder token, Intent intent) {
            BindServiceData s = new BindServiceData();
            s.token = token;
            s.intent = intent;

            sendMessage(H.UNBIND_SERVICE, s);
        }

        public final void scheduleServiceArgs(IBinder token, boolean taskRemoved, int startId,
            int flags ,Intent args) {
            ServiceArgsData s = new ServiceArgsData();
            s.token = token;
            s.taskRemoved = taskRemoved;
            s.startId = startId;
            s.flags = flags;
            s.args = args;

            sendMessage(H.SERVICE_ARGS, s);
        }

        public final void scheduleStopService(IBinder token) {
            sendMessage(H.STOP_SERVICE, token);
        }

        public final void bindApplication(String processName, ApplicationInfo appInfo,
                List<ProviderInfo> providers, ComponentName instrumentationName,
                ProfilerInfo profilerInfo, Bundle instrumentationArgs,
                IInstrumentationWatcher instrumentationWatcher,
                IUiAutomationConnection instrumentationUiConnection, int debugMode,
                boolean enableBinderTracking, boolean trackAllocation,
                boolean isRestrictedBackupMode, boolean persistent, Configuration config,
                CompatibilityInfo compatInfo, Map<String, IBinder> services, Bundle coreSettings) {

            if (services != null) {
                // Setup the service cache in the ServiceManager
                ServiceManager.initServiceCache(services);
            }

            setCoreSettings(coreSettings);

            AppBindData data = new AppBindData();
            data.processName = processName;
            data.appInfo = appInfo;
            data.providers = providers;
            data.instrumentationName = instrumentationName;
            data.instrumentationArgs = instrumentationArgs;
            data.instrumentationWatcher = instrumentationWatcher;
            data.instrumentationUiAutomationConnection = instrumentationUiConnection;
            data.debugMode = debugMode;
            data.enableBinderTracking = enableBinderTracking;
            data.trackAllocation = trackAllocation;
            data.restrictedBackupMode = isRestrictedBackupMode;
            data.persistent = persistent;
            data.config = config;
            data.compatInfo = compatInfo;
            data.initProfilerInfo = profilerInfo;
            sendMessage(H.BIND_APPLICATION, data);
        }

        public final void scheduleExit() {
            sendMessage(H.EXIT_APPLICATION, null);
        }

        public final void scheduleSuicide() {
            sendMessage(H.SUICIDE, null);
        }

        public void scheduleConfigurationChanged(Configuration config) {
            updatePendingConfiguration(config);
            sendMessage(H.CONFIGURATION_CHANGED, config);
        }

        public void updateTimeZone() {
            TimeZone.setDefault(null);
        }

        public void clearDnsCache() {
            // a non-standard API to get this to libcore
            InetAddress.clearDnsCache();
            // Allow libcore to perform the necessary actions as it sees fit upon a network
            // configuration change.
            NetworkEventDispatcher.getInstance().onNetworkConfigurationChanged();
        }

        public void setHttpProxy(String host, String port, String exclList, Uri pacFileUrl) {
            final ConnectivityManager cm = ConnectivityManager.from(getSystemContext());
            final Network network = cm.getBoundNetworkForProcess();
            if (network != null) {
                Proxy.setHttpProxySystemProperty(cm.getDefaultProxy());
            } else {
                Proxy.setHttpProxySystemProperty(host, port, exclList, pacFileUrl);
            }
        }

        public void processInBackground() {
            mH.removeMessages(H.GC_WHEN_IDLE);
            mH.sendMessage(mH.obtainMessage(H.GC_WHEN_IDLE));
        }

        public void dumpService(FileDescriptor fd, IBinder servicetoken, String[] args) {
            DumpComponentInfo data = new DumpComponentInfo();
            try {
                data.fd = ParcelFileDescriptor.dup(fd);
                data.token = servicetoken;
                data.args = args;
                sendMessage(H.DUMP_SERVICE, data, 0, 0, true /*async*/);
            } catch (IOException e) {
                Slog.w(TAG, "dumpService failed", e);
            }
        }

        // This function exists to make sure all receiver dispatching is
        // correctly ordered, since these are one-way calls and the binder driver
        // applies transaction ordering per object for such calls.
        public void scheduleRegisteredReceiver(IIntentReceiver receiver, Intent intent,
                int resultCode, String dataStr, Bundle extras, boolean ordered,
                boolean sticky, int sendingUser, int processState) throws RemoteException {
            updateProcessState(processState, false);
            receiver.performReceive(intent, resultCode, dataStr, extras, ordered,
                    sticky, sendingUser);
        }

        @Override
        public void scheduleLowMemory() {
            sendMessage(H.LOW_MEMORY, null);
        }

        @Override
        public void scheduleActivityConfigurationChanged(
                IBinder token, Configuration overrideConfig, boolean reportToActivity) {
            sendMessage(H.ACTIVITY_CONFIGURATION_CHANGED,
                    new ActivityConfigChangeData(token, overrideConfig), reportToActivity ? 1 : 0);
        }

        @Override
        public void profilerControl(boolean start, ProfilerInfo profilerInfo, int profileType) {
            sendMessage(H.PROFILER_CONTROL, profilerInfo, start ? 1 : 0, profileType);
        }

        public void dumpHeap(boolean managed, String path, ParcelFileDescriptor fd) {
            DumpHeapData dhd = new DumpHeapData();
            dhd.path = path;
            dhd.fd = fd;
            sendMessage(H.DUMP_HEAP, dhd, managed ? 1 : 0, 0, true /*async*/);
        }

        public void setSchedulingGroup(int group) {
            // Note: do this immediately, since going into the foreground
            // should happen regardless of what pending work we have to do
            // and the activity manager will wait for us to report back that
            // we are done before sending us to the background.
            try {
                Process.setProcessGroup(Process.myPid(), group);
            } catch (Exception e) {
                Slog.w(TAG, "Failed setting process group to " + group, e);
            }
        }

        public void dispatchPackageBroadcast(int cmd, String[] packages) {
            sendMessage(H.DISPATCH_PACKAGE_BROADCAST, packages, cmd);
        }

        public void scheduleCrash(String msg) {
            sendMessage(H.SCHEDULE_CRASH, msg);
        }

        public void dumpActivity(FileDescriptor fd, IBinder activitytoken,
                String prefix, String[] args) {
            DumpComponentInfo data = new DumpComponentInfo();
            try {
                data.fd = ParcelFileDescriptor.dup(fd);
                data.token = activitytoken;
                data.prefix = prefix;
                data.args = args;
                sendMessage(H.DUMP_ACTIVITY, data, 0, 0, true /*async*/);
            } catch (IOException e) {
                Slog.w(TAG, "dumpActivity failed", e);
            }
        }

        public void dumpProvider(FileDescriptor fd, IBinder providertoken,
                String[] args) {
            DumpComponentInfo data = new DumpComponentInfo();
            try {
                data.fd = ParcelFileDescriptor.dup(fd);
                data.token = providertoken;
                data.args = args;
                sendMessage(H.DUMP_PROVIDER, data, 0, 0, true /*async*/);
            } catch (IOException e) {
                Slog.w(TAG, "dumpProvider failed", e);
            }
        }

        @Override
        public void dumpMemInfo(FileDescriptor fd, Debug.MemoryInfo mem, boolean checkin,
                boolean dumpFullInfo, boolean dumpDalvik, boolean dumpSummaryOnly,
                boolean dumpUnreachable, String[] args) {
            FileOutputStream fout = new FileOutputStream(fd);
            PrintWriter pw = new FastPrintWriter(fout);
            try {
                dumpMemInfo(pw, mem, checkin, dumpFullInfo, dumpDalvik, dumpSummaryOnly, dumpUnreachable);
            } finally {
                pw.flush();
            }
        }

        private void dumpMemInfo(PrintWriter pw, Debug.MemoryInfo memInfo, boolean checkin,
                boolean dumpFullInfo, boolean dumpDalvik, boolean dumpSummaryOnly, boolean dumpUnreachable) {
            long nativeMax = Debug.getNativeHeapSize() / 1024;
            long nativeAllocated = Debug.getNativeHeapAllocatedSize() / 1024;
            long nativeFree = Debug.getNativeHeapFreeSize() / 1024;

            Runtime runtime = Runtime.getRuntime();
            runtime.gc();  // Do GC since countInstancesOfClass counts unreachable objects.
            long dalvikMax = runtime.totalMemory() / 1024;
            long dalvikFree = runtime.freeMemory() / 1024;
            long dalvikAllocated = dalvikMax - dalvikFree;

            Class[] classesToCount = new Class[] {
                    ContextImpl.class,
                    Activity.class,
                    WebView.class,
                    OpenSSLSocketImpl.class
            };
            long[] instanceCounts = VMDebug.countInstancesOfClasses(classesToCount, true);
            long appContextInstanceCount = instanceCounts[0];
            long activityInstanceCount = instanceCounts[1];
            long webviewInstanceCount = instanceCounts[2];
            long openSslSocketCount = instanceCounts[3];

            long viewInstanceCount = ViewDebug.getViewInstanceCount();
            long viewRootInstanceCount = ViewDebug.getViewRootImplCount();
            int globalAssetCount = AssetManager.getGlobalAssetCount();
            int globalAssetManagerCount = AssetManager.getGlobalAssetManagerCount();
            int binderLocalObjectCount = Debug.getBinderLocalObjectCount();
            int binderProxyObjectCount = Debug.getBinderProxyObjectCount();
            int binderDeathObjectCount = Debug.getBinderDeathObjectCount();
            long parcelSize = Parcel.getGlobalAllocSize();
            long parcelCount = Parcel.getGlobalAllocCount();
            SQLiteDebug.PagerStats stats = SQLiteDebug.getDatabaseInfo();

            dumpMemInfoTable(pw, memInfo, checkin, dumpFullInfo, dumpDalvik, dumpSummaryOnly,
                    Process.myPid(),
                    (mBoundApplication != null) ? mBoundApplication.processName : "unknown",
                    nativeMax, nativeAllocated, nativeFree,
                    dalvikMax, dalvikAllocated, dalvikFree);

            if (checkin) {
                // NOTE: if you change anything significant below, also consider changing
                // ACTIVITY_THREAD_CHECKIN_VERSION.

                // Object counts
                pw.print(viewInstanceCount); pw.print(',');
                pw.print(viewRootInstanceCount); pw.print(',');
                pw.print(appContextInstanceCount); pw.print(',');
                pw.print(activityInstanceCount); pw.print(',');

                pw.print(globalAssetCount); pw.print(',');
                pw.print(globalAssetManagerCount); pw.print(',');
                pw.print(binderLocalObjectCount); pw.print(',');
                pw.print(binderProxyObjectCount); pw.print(',');

                pw.print(binderDeathObjectCount); pw.print(',');
                pw.print(openSslSocketCount); pw.print(',');

                // SQL
                pw.print(stats.memoryUsed / 1024); pw.print(',');
                pw.print(stats.memoryUsed / 1024); pw.print(',');
                pw.print(stats.pageCacheOverflow / 1024); pw.print(',');
                pw.print(stats.largestMemAlloc / 1024);
                for (int i = 0; i < stats.dbStats.size(); i++) {
                    DbStats dbStats = stats.dbStats.get(i);
                    pw.print(','); pw.print(dbStats.dbName);
                    pw.print(','); pw.print(dbStats.pageSize);
                    pw.print(','); pw.print(dbStats.dbSize);
                    pw.print(','); pw.print(dbStats.lookaside);
                    pw.print(','); pw.print(dbStats.cache);
                    pw.print(','); pw.print(dbStats.cache);
                }
                pw.println();

                return;
            }

            pw.println(" ");
            pw.println(" Objects");
            printRow(pw, TWO_COUNT_COLUMNS, "Views:", viewInstanceCount, "ViewRootImpl:",
                    viewRootInstanceCount);

            printRow(pw, TWO_COUNT_COLUMNS, "AppContexts:", appContextInstanceCount,
                    "Activities:", activityInstanceCount);

            printRow(pw, TWO_COUNT_COLUMNS, "Assets:", globalAssetCount,
                    "AssetManagers:", globalAssetManagerCount);

            printRow(pw, TWO_COUNT_COLUMNS, "Local Binders:", binderLocalObjectCount,
                    "Proxy Binders:", binderProxyObjectCount);
            printRow(pw, TWO_COUNT_COLUMNS, "Parcel memory:", parcelSize/1024,
                    "Parcel count:", parcelCount);
            printRow(pw, TWO_COUNT_COLUMNS, "Death Recipients:", binderDeathObjectCount,
                    "OpenSSL Sockets:", openSslSocketCount);
            printRow(pw, ONE_COUNT_COLUMN, "WebViews:", webviewInstanceCount);

            // SQLite mem info
            pw.println(" ");
            pw.println(" SQL");
            printRow(pw, ONE_COUNT_COLUMN, "MEMORY_USED:", stats.memoryUsed / 1024);
            printRow(pw, TWO_COUNT_COLUMNS, "PAGECACHE_OVERFLOW:",
                    stats.pageCacheOverflow / 1024, "MALLOC_SIZE:", stats.largestMemAlloc / 1024);
            pw.println(" ");
            int N = stats.dbStats.size();
            if (N > 0) {
                pw.println(" DATABASES");
                printRow(pw, "  %8s %8s %14s %14s  %s", "pgsz", "dbsz", "Lookaside(b)", "cache",
                        "Dbname");
                for (int i = 0; i < N; i++) {
                    DbStats dbStats = stats.dbStats.get(i);
                    printRow(pw, DB_INFO_FORMAT,
                            (dbStats.pageSize > 0) ? String.valueOf(dbStats.pageSize) : " ",
                            (dbStats.dbSize > 0) ? String.valueOf(dbStats.dbSize) : " ",
                            (dbStats.lookaside > 0) ? String.valueOf(dbStats.lookaside) : " ",
                            dbStats.cache, dbStats.dbName);
                }
            }

            // Asset details.
            String assetAlloc = AssetManager.getAssetAllocations();
            if (assetAlloc != null) {
                pw.println(" ");
                pw.println(" Asset Allocations");
                pw.print(assetAlloc);
            }

            // Unreachable native memory
            if (dumpUnreachable) {
                boolean showContents = ((mBoundApplication != null)
                    && ((mBoundApplication.appInfo.flags&ApplicationInfo.FLAG_DEBUGGABLE) != 0))
                    || android.os.Build.IS_DEBUGGABLE;
                pw.println(" ");
                pw.println(" Unreachable memory");
                pw.print(Debug.getUnreachableMemory(100, showContents));
            }
        }

        @Override
        public void dumpGfxInfo(FileDescriptor fd, String[] args) {
            dumpGraphicsInfo(fd);
            WindowManagerGlobal.getInstance().dumpGfxInfo(fd, args);
        }

        private void dumpDatabaseInfo(FileDescriptor fd, String[] args) {
            PrintWriter pw = new FastPrintWriter(new FileOutputStream(fd));
            PrintWriterPrinter printer = new PrintWriterPrinter(pw);
            SQLiteDebug.dump(printer, args);
            pw.flush();
        }

        @Override
        public void dumpDbInfo(final FileDescriptor fd, final String[] args) {
            if (mSystemThread) {
                // Ensure this invocation is asynchronous to prevent writer waiting if buffer cannot
                // be consumed. But it must duplicate the file descriptor first, since caller might
                // be closing it.
                final ParcelFileDescriptor dup;
                try {
                    dup = ParcelFileDescriptor.dup(fd);
                } catch (IOException e) {
                    Log.w(TAG, "Could not dup FD " + fd.getInt$());
                    return;
                }

                AsyncTask.THREAD_POOL_EXECUTOR.execute(new Runnable() {
                    @Override
                    public void run() {
                        try {
                            dumpDatabaseInfo(dup.getFileDescriptor(), args);
                        } finally {
                            IoUtils.closeQuietly(dup);
                        }
                    }
                });
            } else {
                dumpDatabaseInfo(fd, args);
            }
        }

        @Override
        public void unstableProviderDied(IBinder provider) {
            sendMessage(H.UNSTABLE_PROVIDER_DIED, provider);
        }

        @Override
        public void requestAssistContextExtras(IBinder activityToken, IBinder requestToken,
                int requestType, int sessionId) {
            RequestAssistContextExtras cmd = new RequestAssistContextExtras();
            cmd.activityToken = activityToken;
            cmd.requestToken = requestToken;
            cmd.requestType = requestType;
            cmd.sessionId = sessionId;
            sendMessage(H.REQUEST_ASSIST_CONTEXT_EXTRAS, cmd);
        }

        public void setCoreSettings(Bundle coreSettings) {
            sendMessage(H.SET_CORE_SETTINGS, coreSettings);
        }

        public void updatePackageCompatibilityInfo(String pkg, CompatibilityInfo info) {
            UpdateCompatibilityData ucd = new UpdateCompatibilityData();
            ucd.pkg = pkg;
            ucd.info = info;
            sendMessage(H.UPDATE_PACKAGE_COMPATIBILITY_INFO, ucd);
        }

        public void scheduleTrimMemory(int level) {
            sendMessage(H.TRIM_MEMORY, null, level);
        }

        public void scheduleTranslucentConversionComplete(IBinder token, boolean drawComplete) {
            sendMessage(H.TRANSLUCENT_CONVERSION_COMPLETE, token, drawComplete ? 1 : 0);
        }

        public void scheduleOnNewActivityOptions(IBinder token, ActivityOptions options) {
            sendMessage(H.ON_NEW_ACTIVITY_OPTIONS,
                    new Pair<IBinder, ActivityOptions>(token, options));
        }

        public void setProcessState(int state) {
            updateProcessState(state, true);
        }

        public void updateProcessState(int processState, boolean fromIpc) {
            synchronized (this) {
                if (mLastProcessState != processState) {
                    mLastProcessState = processState;
                    // Update Dalvik state based on ActivityManager.PROCESS_STATE_* constants.
                    final int DALVIK_PROCESS_STATE_JANK_PERCEPTIBLE = 0;
                    final int DALVIK_PROCESS_STATE_JANK_IMPERCEPTIBLE = 1;
                    int dalvikProcessState = DALVIK_PROCESS_STATE_JANK_IMPERCEPTIBLE;
                    // TODO: Tune this since things like gmail sync are important background but not jank perceptible.
                    if (processState <= ActivityManager.PROCESS_STATE_IMPORTANT_FOREGROUND) {
                        dalvikProcessState = DALVIK_PROCESS_STATE_JANK_PERCEPTIBLE;
                    }
                    VMRuntime.getRuntime().updateProcessState(dalvikProcessState);
                    if (false) {
                        Slog.i(TAG, "******************* PROCESS STATE CHANGED TO: " + processState
                                + (fromIpc ? " (from ipc": ""));
                    }
                }
            }
        }

        @Override
        public void scheduleInstallProvider(ProviderInfo provider) {
            sendMessage(H.INSTALL_PROVIDER, provider);
        }

        @Override
        public final void updateTimePrefs(boolean is24Hour) {
            DateFormat.set24HourTimePref(is24Hour);
        }

        @Override
        public void scheduleCancelVisibleBehind(IBinder token) {
            sendMessage(H.CANCEL_VISIBLE_BEHIND, token);
        }

        @Override
        public void scheduleBackgroundVisibleBehindChanged(IBinder token, boolean visible) {
            sendMessage(H.BACKGROUND_VISIBLE_BEHIND_CHANGED, token, visible ? 1 : 0);
        }

        @Override
        public void scheduleEnterAnimationComplete(IBinder token) {
            sendMessage(H.ENTER_ANIMATION_COMPLETE, token);
        }

        @Override
        public void notifyCleartextNetwork(byte[] firstPacket) {
            if (StrictMode.vmCleartextNetworkEnabled()) {
                StrictMode.onCleartextNetworkDetected(firstPacket);
            }
        }

        @Override
        public void startBinderTracking() {
            sendMessage(H.START_BINDER_TRACKING, null);
        }

        @Override
        public void stopBinderTrackingAndDump(FileDescriptor fd) {
            try {
                sendMessage(H.STOP_BINDER_TRACKING_AND_DUMP, ParcelFileDescriptor.dup(fd));
            } catch (IOException e) {
            }
        }

        @Override
        public void scheduleMultiWindowModeChanged(IBinder token, boolean isInMultiWindowMode)
                throws RemoteException {
            sendMessage(H.MULTI_WINDOW_MODE_CHANGED, token, isInMultiWindowMode ? 1 : 0);
        }

        @Override
        public void schedulePictureInPictureModeChanged(IBinder token, boolean isInPipMode)
                throws RemoteException {
            sendMessage(H.PICTURE_IN_PICTURE_MODE_CHANGED, token, isInPipMode ? 1 : 0);
        }

        @Override
        public void scheduleLocalVoiceInteractionStarted(IBinder token,
                IVoiceInteractor voiceInteractor) throws RemoteException {
            SomeArgs args = SomeArgs.obtain();
            args.arg1 = token;
            args.arg2 = voiceInteractor;
            sendMessage(H.LOCAL_VOICE_INTERACTION_STARTED, args);
        }
    }

    private int getLifecycleSeq() {
        synchronized (mResourcesManager) {
            return mLifecycleSeq++;
        }
    }

    private class H extends Handler {
        public static final int LAUNCH_ACTIVITY         = 100;
        public static final int PAUSE_ACTIVITY          = 101;
        public static final int PAUSE_ACTIVITY_FINISHING= 102;
        public static final int STOP_ACTIVITY_SHOW      = 103;
        public static final int STOP_ACTIVITY_HIDE      = 104;
        public static final int SHOW_WINDOW             = 105;
        public static final int HIDE_WINDOW             = 106;
        public static final int RESUME_ACTIVITY         = 107;
        public static final int SEND_RESULT             = 108;
        public static final int DESTROY_ACTIVITY        = 109;
        public static final int BIND_APPLICATION        = 110;
        public static final int EXIT_APPLICATION        = 111;
        public static final int NEW_INTENT              = 112;
        public static final int RECEIVER                = 113;
        public static final int CREATE_SERVICE          = 114;
        public static final int SERVICE_ARGS            = 115;
        public static final int STOP_SERVICE            = 116;

        public static final int CONFIGURATION_CHANGED   = 118;
        public static final int CLEAN_UP_CONTEXT        = 119;
        public static final int GC_WHEN_IDLE            = 120;
        public static final int BIND_SERVICE            = 121;
        public static final int UNBIND_SERVICE          = 122;
        public static final int DUMP_SERVICE            = 123;
        public static final int LOW_MEMORY              = 124;
        public static final int ACTIVITY_CONFIGURATION_CHANGED = 125;
        public static final int RELAUNCH_ACTIVITY       = 126;
        public static final int PROFILER_CONTROL        = 127;
        public static final int CREATE_BACKUP_AGENT     = 128;
        public static final int DESTROY_BACKUP_AGENT    = 129;
        public static final int SUICIDE                 = 130;
        public static final int REMOVE_PROVIDER         = 131;
        public static final int ENABLE_JIT              = 132;
        public static final int DISPATCH_PACKAGE_BROADCAST = 133;
        public static final int SCHEDULE_CRASH          = 134;
        public static final int DUMP_HEAP               = 135;
        public static final int DUMP_ACTIVITY           = 136;
        public static final int SLEEPING                = 137;
        public static final int SET_CORE_SETTINGS       = 138;
        public static final int UPDATE_PACKAGE_COMPATIBILITY_INFO = 139;
        public static final int TRIM_MEMORY             = 140;
        public static final int DUMP_PROVIDER           = 141;
        public static final int UNSTABLE_PROVIDER_DIED  = 142;
        public static final int REQUEST_ASSIST_CONTEXT_EXTRAS = 143;
        public static final int TRANSLUCENT_CONVERSION_COMPLETE = 144;
        public static final int INSTALL_PROVIDER        = 145;
        public static final int ON_NEW_ACTIVITY_OPTIONS = 146;
        public static final int CANCEL_VISIBLE_BEHIND = 147;
        public static final int BACKGROUND_VISIBLE_BEHIND_CHANGED = 148;
        public static final int ENTER_ANIMATION_COMPLETE = 149;
        public static final int START_BINDER_TRACKING = 150;
        public static final int STOP_BINDER_TRACKING_AND_DUMP = 151;
        public static final int MULTI_WINDOW_MODE_CHANGED = 152;
        public static final int PICTURE_IN_PICTURE_MODE_CHANGED = 153;
        public static final int LOCAL_VOICE_INTERACTION_STARTED = 154;

        String codeToString(int code) {
            if (DEBUG_MESSAGES) {
                switch (code) {
                    case LAUNCH_ACTIVITY: return "LAUNCH_ACTIVITY";
                    case PAUSE_ACTIVITY: return "PAUSE_ACTIVITY";
                    case PAUSE_ACTIVITY_FINISHING: return "PAUSE_ACTIVITY_FINISHING";
                    case STOP_ACTIVITY_SHOW: return "STOP_ACTIVITY_SHOW";
                    case STOP_ACTIVITY_HIDE: return "STOP_ACTIVITY_HIDE";
                    case SHOW_WINDOW: return "SHOW_WINDOW";
                    case HIDE_WINDOW: return "HIDE_WINDOW";
                    case RESUME_ACTIVITY: return "RESUME_ACTIVITY";
                    case SEND_RESULT: return "SEND_RESULT";
                    case DESTROY_ACTIVITY: return "DESTROY_ACTIVITY";
                    case BIND_APPLICATION: return "BIND_APPLICATION";
                    case EXIT_APPLICATION: return "EXIT_APPLICATION";
                    case NEW_INTENT: return "NEW_INTENT";
                    case RECEIVER: return "RECEIVER";
                    case CREATE_SERVICE: return "CREATE_SERVICE";
                    case SERVICE_ARGS: return "SERVICE_ARGS";
                    case STOP_SERVICE: return "STOP_SERVICE";
                    case CONFIGURATION_CHANGED: return "CONFIGURATION_CHANGED";
                    case CLEAN_UP_CONTEXT: return "CLEAN_UP_CONTEXT";
                    case GC_WHEN_IDLE: return "GC_WHEN_IDLE";
                    case BIND_SERVICE: return "BIND_SERVICE";
                    case UNBIND_SERVICE: return "UNBIND_SERVICE";
                    case DUMP_SERVICE: return "DUMP_SERVICE";
                    case LOW_MEMORY: return "LOW_MEMORY";
                    case ACTIVITY_CONFIGURATION_CHANGED: return "ACTIVITY_CONFIGURATION_CHANGED";
                    case RELAUNCH_ACTIVITY: return "RELAUNCH_ACTIVITY";
                    case PROFILER_CONTROL: return "PROFILER_CONTROL";
                    case CREATE_BACKUP_AGENT: return "CREATE_BACKUP_AGENT";
                    case DESTROY_BACKUP_AGENT: return "DESTROY_BACKUP_AGENT";
                    case SUICIDE: return "SUICIDE";
                    case REMOVE_PROVIDER: return "REMOVE_PROVIDER";
                    case ENABLE_JIT: return "ENABLE_JIT";
                    case DISPATCH_PACKAGE_BROADCAST: return "DISPATCH_PACKAGE_BROADCAST";
                    case SCHEDULE_CRASH: return "SCHEDULE_CRASH";
                    case DUMP_HEAP: return "DUMP_HEAP";
                    case DUMP_ACTIVITY: return "DUMP_ACTIVITY";
                    case SLEEPING: return "SLEEPING";
                    case SET_CORE_SETTINGS: return "SET_CORE_SETTINGS";
                    case UPDATE_PACKAGE_COMPATIBILITY_INFO: return "UPDATE_PACKAGE_COMPATIBILITY_INFO";
                    case TRIM_MEMORY: return "TRIM_MEMORY";
                    case DUMP_PROVIDER: return "DUMP_PROVIDER";
                    case UNSTABLE_PROVIDER_DIED: return "UNSTABLE_PROVIDER_DIED";
                    case REQUEST_ASSIST_CONTEXT_EXTRAS: return "REQUEST_ASSIST_CONTEXT_EXTRAS";
                    case TRANSLUCENT_CONVERSION_COMPLETE: return "TRANSLUCENT_CONVERSION_COMPLETE";
                    case INSTALL_PROVIDER: return "INSTALL_PROVIDER";
                    case ON_NEW_ACTIVITY_OPTIONS: return "ON_NEW_ACTIVITY_OPTIONS";
                    case CANCEL_VISIBLE_BEHIND: return "CANCEL_VISIBLE_BEHIND";
                    case BACKGROUND_VISIBLE_BEHIND_CHANGED: return "BACKGROUND_VISIBLE_BEHIND_CHANGED";
                    case ENTER_ANIMATION_COMPLETE: return "ENTER_ANIMATION_COMPLETE";
                    case MULTI_WINDOW_MODE_CHANGED: return "MULTI_WINDOW_MODE_CHANGED";
                    case PICTURE_IN_PICTURE_MODE_CHANGED: return "PICTURE_IN_PICTURE_MODE_CHANGED";
                    case LOCAL_VOICE_INTERACTION_STARTED: return "LOCAL_VOICE_INTERACTION_STARTED";
                }
            }
            return Integer.toString(code);
        }
        public void handleMessage(Message msg) {
            if (DEBUG_MESSAGES) Slog.v(TAG, ">>> handling: " + codeToString(msg.what));
            switch (msg.what) {
                case LAUNCH_ACTIVITY: {
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityStart");
                    final ActivityClientRecord r = (ActivityClientRecord) msg.obj;

                    r.packageInfo = getPackageInfoNoCheck(
                            r.activityInfo.applicationInfo, r.compatInfo);
                    handleLaunchActivity(r, null, "LAUNCH_ACTIVITY");
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                } break;
                case RELAUNCH_ACTIVITY: {
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityRestart");
                    ActivityClientRecord r = (ActivityClientRecord)msg.obj;
                    handleRelaunchActivity(r);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                } break;
                case PAUSE_ACTIVITY: {
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityPause");
                    SomeArgs args = (SomeArgs) msg.obj;
                    handlePauseActivity((IBinder) args.arg1, false,
                            (args.argi1 & USER_LEAVING) != 0, args.argi2,
                            (args.argi1 & DONT_REPORT) != 0, args.argi3);
                    maybeSnapshot();
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                } break;
                case PAUSE_ACTIVITY_FINISHING: {
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityPause");
                    SomeArgs args = (SomeArgs) msg.obj;
                    handlePauseActivity((IBinder) args.arg1, true, (args.argi1 & USER_LEAVING) != 0,
                            args.argi2, (args.argi1 & DONT_REPORT) != 0, args.argi3);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                } break;
                case STOP_ACTIVITY_SHOW: {
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityStop");
                    SomeArgs args = (SomeArgs) msg.obj;
                    handleStopActivity((IBinder) args.arg1, true, args.argi2, args.argi3);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                } break;
                case STOP_ACTIVITY_HIDE: {
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityStop");
                    SomeArgs args = (SomeArgs) msg.obj;
                    handleStopActivity((IBinder) args.arg1, false, args.argi2, args.argi3);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                } break;
                case SHOW_WINDOW:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityShowWindow");
                    handleWindowVisibility((IBinder)msg.obj, true);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case HIDE_WINDOW:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityHideWindow");
                    handleWindowVisibility((IBinder)msg.obj, false);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case RESUME_ACTIVITY:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityResume");
                    SomeArgs args = (SomeArgs) msg.obj;
                    handleResumeActivity((IBinder) args.arg1, true, args.argi1 != 0, true,
                            args.argi3, "RESUME_ACTIVITY");
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case SEND_RESULT:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityDeliverResult");
                    handleSendResult((ResultData)msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case DESTROY_ACTIVITY:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityDestroy");
                    handleDestroyActivity((IBinder)msg.obj, msg.arg1 != 0,
                            msg.arg2, false);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case BIND_APPLICATION:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "bindApplication");
                    AppBindData data = (AppBindData)msg.obj;
                    handleBindApplication(data);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case EXIT_APPLICATION:
                    if (mInitialApplication != null) {
                        mInitialApplication.onTerminate();
                    }
                    Looper.myLooper().quit();
                    break;
                case NEW_INTENT:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityNewIntent");
                    handleNewIntent((NewIntentData)msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case RECEIVER:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "broadcastReceiveComp");
                    handleReceiver((ReceiverData)msg.obj);
                    maybeSnapshot();
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case CREATE_SERVICE:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, ("serviceCreate: " + String.valueOf(msg.obj)));
                    handleCreateService((CreateServiceData)msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case BIND_SERVICE:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "serviceBind");
                    handleBindService((BindServiceData)msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case UNBIND_SERVICE:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "serviceUnbind");
                    handleUnbindService((BindServiceData)msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case SERVICE_ARGS:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, ("serviceStart: " + String.valueOf(msg.obj)));
                    handleServiceArgs((ServiceArgsData)msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case STOP_SERVICE:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "serviceStop");
                    handleStopService((IBinder)msg.obj);
                    maybeSnapshot();
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case CONFIGURATION_CHANGED:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "configChanged");
                    mCurDefaultDisplayDpi = ((Configuration)msg.obj).densityDpi;
                    mUpdatingSystemConfig = true;
                    handleConfigurationChanged((Configuration)msg.obj, null);
                    mUpdatingSystemConfig = false;
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case CLEAN_UP_CONTEXT:
                    ContextCleanupInfo cci = (ContextCleanupInfo)msg.obj;
                    cci.context.performFinalCleanup(cci.who, cci.what);
                    break;
                case GC_WHEN_IDLE:
                    scheduleGcIdler();
                    break;
                case DUMP_SERVICE:
                    handleDumpService((DumpComponentInfo)msg.obj);
                    break;
                case LOW_MEMORY:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "lowMemory");
                    handleLowMemory();
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case ACTIVITY_CONFIGURATION_CHANGED:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "activityConfigChanged");
                    handleActivityConfigurationChanged((ActivityConfigChangeData) msg.obj,
                            msg.arg1 == 1 ? REPORT_TO_ACTIVITY : !REPORT_TO_ACTIVITY);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case PROFILER_CONTROL:
                    handleProfilerControl(msg.arg1 != 0, (ProfilerInfo)msg.obj, msg.arg2);
                    break;
                case CREATE_BACKUP_AGENT:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "backupCreateAgent");
                    handleCreateBackupAgent((CreateBackupAgentData)msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case DESTROY_BACKUP_AGENT:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "backupDestroyAgent");
                    handleDestroyBackupAgent((CreateBackupAgentData)msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case SUICIDE:
                    Process.killProcess(Process.myPid());
                    break;
                case REMOVE_PROVIDER:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "providerRemove");
                    completeRemoveProvider((ProviderRefCount)msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case ENABLE_JIT:
                    ensureJitEnabled();
                    break;
                case DISPATCH_PACKAGE_BROADCAST:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "broadcastPackage");
                    handleDispatchPackageBroadcast(msg.arg1, (String[])msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case SCHEDULE_CRASH:
                    throw new RemoteServiceException((String)msg.obj);
                case DUMP_HEAP:
                    handleDumpHeap(msg.arg1 != 0, (DumpHeapData)msg.obj);
                    break;
                case DUMP_ACTIVITY:
                    handleDumpActivity((DumpComponentInfo)msg.obj);
                    break;
                case DUMP_PROVIDER:
                    handleDumpProvider((DumpComponentInfo)msg.obj);
                    break;
                case SLEEPING:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "sleeping");
                    handleSleeping((IBinder)msg.obj, msg.arg1 != 0);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case SET_CORE_SETTINGS:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "setCoreSettings");
                    handleSetCoreSettings((Bundle) msg.obj);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case UPDATE_PACKAGE_COMPATIBILITY_INFO:
                    handleUpdatePackageCompatibilityInfo((UpdateCompatibilityData)msg.obj);
                    break;
                case TRIM_MEMORY:
                    Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "trimMemory");
                    handleTrimMemory(msg.arg1);
                    Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                    break;
                case UNSTABLE_PROVIDER_DIED:
                    handleUnstableProviderDied((IBinder)msg.obj, false);
                    break;
                case REQUEST_ASSIST_CONTEXT_EXTRAS:
                    handleRequestAssistContextExtras((RequestAssistContextExtras)msg.obj);
                    break;
                case TRANSLUCENT_CONVERSION_COMPLETE:
                    handleTranslucentConversionComplete((IBinder)msg.obj, msg.arg1 == 1);
                    break;
                case INSTALL_PROVIDER:
                    handleInstallProvider((ProviderInfo) msg.obj);
                    break;
                case ON_NEW_ACTIVITY_OPTIONS:
                    Pair<IBinder, ActivityOptions> pair = (Pair<IBinder, ActivityOptions>) msg.obj;
                    onNewActivityOptions(pair.first, pair.second);
                    break;
                case CANCEL_VISIBLE_BEHIND:
                    handleCancelVisibleBehind((IBinder) msg.obj);
                    break;
                case BACKGROUND_VISIBLE_BEHIND_CHANGED:
                    handleOnBackgroundVisibleBehindChanged((IBinder) msg.obj, msg.arg1 > 0);
                    break;
                case ENTER_ANIMATION_COMPLETE:
                    handleEnterAnimationComplete((IBinder) msg.obj);
                    break;
                case START_BINDER_TRACKING:
                    handleStartBinderTracking();
                    break;
                case STOP_BINDER_TRACKING_AND_DUMP:
                    handleStopBinderTrackingAndDump((ParcelFileDescriptor) msg.obj);
                    break;
                case MULTI_WINDOW_MODE_CHANGED:
                    handleMultiWindowModeChanged((IBinder) msg.obj, msg.arg1 == 1);
                    break;
                case PICTURE_IN_PICTURE_MODE_CHANGED:
                    handlePictureInPictureModeChanged((IBinder) msg.obj, msg.arg1 == 1);
                    break;
                case LOCAL_VOICE_INTERACTION_STARTED:
                    handleLocalVoiceInteractionStarted((IBinder) ((SomeArgs) msg.obj).arg1,
                            (IVoiceInteractor) ((SomeArgs) msg.obj).arg2);
                    break;
            }
            Object obj = msg.obj;
            if (obj instanceof SomeArgs) {
                ((SomeArgs) obj).recycle();
            }
            if (DEBUG_MESSAGES) Slog.v(TAG, "<<< done: " + codeToString(msg.what));
        }

        private void maybeSnapshot() {
            if (mBoundApplication != null && SamplingProfilerIntegration.isEnabled()) {
                // convert the *private* ActivityThread.PackageInfo to *public* known
                // android.content.pm.PackageInfo
                String packageName = mBoundApplication.info.mPackageName;
                android.content.pm.PackageInfo packageInfo = null;
                try {
                    Context context = getSystemContext();
                    if(context == null) {
                        Log.e(TAG, "cannot get a valid context");
                        return;
                    }
                    PackageManager pm = context.getPackageManager();
                    if(pm == null) {
                        Log.e(TAG, "cannot get a valid PackageManager");
                        return;
                    }
                    packageInfo = pm.getPackageInfo(
                            packageName, PackageManager.GET_ACTIVITIES);
                } catch (NameNotFoundException e) {
                    Log.e(TAG, "cannot get package info for " + packageName, e);
                }
                SamplingProfilerIntegration.writeSnapshot(mBoundApplication.processName, packageInfo);
            }
        }
    }

    private class Idler implements MessageQueue.IdleHandler {
        @Override
        public final boolean queueIdle() {
            ActivityClientRecord a = mNewActivities;
            boolean stopProfiling = false;
            if (mBoundApplication != null && mProfiler.profileFd != null
                    && mProfiler.autoStopProfiler) {
                stopProfiling = true;
            }
            if (a != null) {
                mNewActivities = null;
                IActivityManager am = ActivityManagerNative.getDefault();
                ActivityClientRecord prev;
                do {
                    if (localLOGV) Slog.v(
                        TAG, "Reporting idle of " + a +
                        " finished=" +
                        (a.activity != null && a.activity.mFinished));
                    if (a.activity != null && !a.activity.mFinished) {
                        try {
                            am.activityIdle(a.token, a.createdConfig, stopProfiling);
                            a.createdConfig = null;
                        } catch (RemoteException ex) {
                            throw ex.rethrowFromSystemServer();
                        }
                    }
                    prev = a;
                    a = a.nextIdle;
                    prev.nextIdle = null;
                } while (a != null);
            }
            if (stopProfiling) {
                mProfiler.stopProfiling();
            }
            ensureJitEnabled();
            return false;
        }
    }

    final class GcIdler implements MessageQueue.IdleHandler {
        @Override
        public final boolean queueIdle() {
            doGcIfNeeded();
            return false;
        }
    }

    public static ActivityThread currentActivityThread() {
        return sCurrentActivityThread;
    }

    public static boolean isSystem() {
        return (sCurrentActivityThread != null) ? sCurrentActivityThread.mSystemThread : false;
    }

    public static String currentOpPackageName() {
        ActivityThread am = currentActivityThread();
        return (am != null && am.getApplication() != null)
                ? am.getApplication().getOpPackageName() : null;
    }

    public static String currentPackageName() {
        ActivityThread am = currentActivityThread();
        return (am != null && am.mBoundApplication != null)
            ? am.mBoundApplication.appInfo.packageName : null;
    }

    public static String currentProcessName() {
        ActivityThread am = currentActivityThread();
        return (am != null && am.mBoundApplication != null)
            ? am.mBoundApplication.processName : null;
    }

    public static Application currentApplication() {
        ActivityThread am = currentActivityThread();
        return am != null ? am.mInitialApplication : null;
    }

    public static IPackageManager getPackageManager() {
        if (sPackageManager != null) {
            //Slog.v("PackageManager", "returning cur default = " + sPackageManager);
            return sPackageManager;
        }
        IBinder b = ServiceManager.getService("package");
        //Slog.v("PackageManager", "default service binder = " + b);
        sPackageManager = IPackageManager.Stub.asInterface(b);
        //Slog.v("PackageManager", "default service = " + sPackageManager);
        return sPackageManager;
    }

    private Configuration mMainThreadConfig = new Configuration();

    Configuration applyConfigCompatMainThread(int displayDensity, Configuration config,
            CompatibilityInfo compat) {
        if (config == null) {
            return null;
        }
        if (!compat.supportsScreen()) {
            mMainThreadConfig.setTo(config);
            config = mMainThreadConfig;
            compat.applyToConfiguration(displayDensity, config);
        }
        return config;
    }

    /**
     * Creates the top level resources for the given package. Will return an existing
     * Resources if one has already been created.
     */
    Resources getTopLevelResources(String resDir, String[] splitResDirs, String[] overlayDirs,
            String[] libDirs, int displayId, LoadedApk pkgInfo) {
        return mResourcesManager.getResources(null, resDir, splitResDirs, overlayDirs, libDirs,
                displayId, null, pkgInfo.getCompatibilityInfo(), pkgInfo.getClassLoader());
    }

    final Handler getHandler() {
        return mH;
    }

    public final LoadedApk getPackageInfo(String packageName, CompatibilityInfo compatInfo,
            int flags) {
        return getPackageInfo(packageName, compatInfo, flags, UserHandle.myUserId());
    }

    public final LoadedApk getPackageInfo(String packageName, CompatibilityInfo compatInfo,
            int flags, int userId) {
        final boolean differentUser = (UserHandle.myUserId() != userId);
        synchronized (mResourcesManager) {
            WeakReference<LoadedApk> ref;
            if (differentUser) {
                // Caching not supported across users
                ref = null;
            } else if ((flags & Context.CONTEXT_INCLUDE_CODE) != 0) {
                ref = mPackages.get(packageName);
            } else {
                ref = mResourcePackages.get(packageName);
            }

            LoadedApk packageInfo = ref != null ? ref.get() : null;
            //Slog.i(TAG, "getPackageInfo " + packageName + ": " + packageInfo);
            //if (packageInfo != null) Slog.i(TAG, "isUptoDate " + packageInfo.mResDir
            //        + ": " + packageInfo.mResources.getAssets().isUpToDate());
            if (packageInfo != null && (packageInfo.mResources == null
                    || packageInfo.mResources.getAssets().isUpToDate())) {
                if (packageInfo.isSecurityViolation()
                        && (flags&Context.CONTEXT_IGNORE_SECURITY) == 0) {
                    throw new SecurityException(
                            "Requesting code from " + packageName
                            + " to be run in process "
                            + mBoundApplication.processName
                            + "/" + mBoundApplication.appInfo.uid);
                }
                return packageInfo;
            }
        }

        ApplicationInfo ai = null;
        try {
            ai = getPackageManager().getApplicationInfo(packageName,
                    PackageManager.GET_SHARED_LIBRARY_FILES
                            | PackageManager.MATCH_DEBUG_TRIAGED_MISSING,
                    userId);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }

        if (ai != null) {
            return getPackageInfo(ai, compatInfo, flags);
        }

        return null;
    }

    public final LoadedApk getPackageInfo(ApplicationInfo ai, CompatibilityInfo compatInfo,
            int flags) {
        boolean includeCode = (flags&Context.CONTEXT_INCLUDE_CODE) != 0;
        boolean securityViolation = includeCode && ai.uid != 0
                && ai.uid != Process.SYSTEM_UID && (mBoundApplication != null
                        ? !UserHandle.isSameApp(ai.uid, mBoundApplication.appInfo.uid)
                        : true);
        boolean registerPackage = includeCode && (flags&Context.CONTEXT_REGISTER_PACKAGE) != 0;
        if ((flags&(Context.CONTEXT_INCLUDE_CODE
                |Context.CONTEXT_IGNORE_SECURITY))
                == Context.CONTEXT_INCLUDE_CODE) {
            if (securityViolation) {
                String msg = "Requesting code from " + ai.packageName
                        + " (with uid " + ai.uid + ")";
                if (mBoundApplication != null) {
                    msg = msg + " to be run in process "
                        + mBoundApplication.processName + " (with uid "
                        + mBoundApplication.appInfo.uid + ")";
                }
                throw new SecurityException(msg);
            }
        }
        return getPackageInfo(ai, compatInfo, null, securityViolation, includeCode,
                registerPackage);
    }

    public final LoadedApk getPackageInfoNoCheck(ApplicationInfo ai,
            CompatibilityInfo compatInfo) {
        return getPackageInfo(ai, compatInfo, null, false, true, false);
    }

    public final LoadedApk peekPackageInfo(String packageName, boolean includeCode) {
        synchronized (mResourcesManager) {
            WeakReference<LoadedApk> ref;
            if (includeCode) {
                ref = mPackages.get(packageName);
            } else {
                ref = mResourcePackages.get(packageName);
            }
            return ref != null ? ref.get() : null;
        }
    }

    private LoadedApk getPackageInfo(ApplicationInfo aInfo, CompatibilityInfo compatInfo,
            ClassLoader baseLoader, boolean securityViolation, boolean includeCode,
            boolean registerPackage) {
        final boolean differentUser = (UserHandle.myUserId() != UserHandle.getUserId(aInfo.uid));
        synchronized (mResourcesManager) {
            WeakReference<LoadedApk> ref;
            if (differentUser) {
                // Caching not supported across users
                ref = null;
            } else if (includeCode) {
                ref = mPackages.get(aInfo.packageName);
            } else {
                ref = mResourcePackages.get(aInfo.packageName);
            }

            LoadedApk packageInfo = ref != null ? ref.get() : null;
            if (packageInfo == null || (packageInfo.mResources != null
                    && !packageInfo.mResources.getAssets().isUpToDate())) {
                if (localLOGV) Slog.v(TAG, (includeCode ? "Loading code package "
                        : "Loading resource-only package ") + aInfo.packageName
                        + " (in " + (mBoundApplication != null
                                ? mBoundApplication.processName : null)
                        + ")");
                packageInfo =
                    new LoadedApk(this, aInfo, compatInfo, baseLoader,
                            securityViolation, includeCode &&
                            (aInfo.flags&ApplicationInfo.FLAG_HAS_CODE) != 0, registerPackage);

                if (mSystemThread && "android".equals(aInfo.packageName)) {
                    packageInfo.installSystemApplicationInfo(aInfo,
                            getSystemContext().mPackageInfo.getClassLoader());
                }

                if (differentUser) {
                    // Caching not supported across users
                } else if (includeCode) {
                    mPackages.put(aInfo.packageName,
                            new WeakReference<LoadedApk>(packageInfo));
                } else {
                    mResourcePackages.put(aInfo.packageName,
                            new WeakReference<LoadedApk>(packageInfo));
                }
            }
            return packageInfo;
        }
    }

    ActivityThread() {
        mResourcesManager = ResourcesManager.getInstance();
    }

    public ApplicationThread getApplicationThread()
    {
        return mAppThread;
    }

    public Instrumentation getInstrumentation()
    {
        return mInstrumentation;
    }

    public boolean isProfiling() {
        return mProfiler != null && mProfiler.profileFile != null
                && mProfiler.profileFd == null;
    }

    public String getProfileFilePath() {
        return mProfiler.profileFile;
    }

    public Looper getLooper() {
        return mLooper;
    }

    public Application getApplication() {
        return mInitialApplication;
    }

    public String getProcessName() {
        return mBoundApplication.processName;
    }

    public ContextImpl getSystemContext() {
        synchronized (this) {
            if (mSystemContext == null) {
                mSystemContext = ContextImpl.createSystemContext(this);
            }
            return mSystemContext;
        }
    }

    public void installSystemApplicationInfo(ApplicationInfo info, ClassLoader classLoader) {
        synchronized (this) {
            getSystemContext().installSystemApplicationInfo(info, classLoader);

            // give ourselves a default profiler
            mProfiler = new Profiler();
        }
    }

    void ensureJitEnabled() {
        if (!mJitEnabled) {
            mJitEnabled = true;
            dalvik.system.VMRuntime.getRuntime().startJitCompilation();
        }
    }

    void scheduleGcIdler() {
        if (!mGcIdlerScheduled) {
            mGcIdlerScheduled = true;
            Looper.myQueue().addIdleHandler(mGcIdler);
        }
        mH.removeMessages(H.GC_WHEN_IDLE);
    }

    void unscheduleGcIdler() {
        if (mGcIdlerScheduled) {
            mGcIdlerScheduled = false;
            Looper.myQueue().removeIdleHandler(mGcIdler);
        }
        mH.removeMessages(H.GC_WHEN_IDLE);
    }

    void doGcIfNeeded() {
        mGcIdlerScheduled = false;
        final long now = SystemClock.uptimeMillis();
        //Slog.i(TAG, "**** WE MIGHT WANT TO GC: then=" + Binder.getLastGcTime()
        //        + "m now=" + now);
        if ((BinderInternal.getLastGcTime()+MIN_TIME_BETWEEN_GCS) < now) {
            //Slog.i(TAG, "**** WE DO, WE DO WANT TO GC!");
            BinderInternal.forceGc("bg");
        }
    }

    private static final String HEAP_FULL_COLUMN
            = "%13s %8s %8s %8s %8s %8s %8s %8s %8s %8s %8s";
    private static final String HEAP_COLUMN
            = "%13s %8s %8s %8s %8s %8s %8s %8s";
    private static final String ONE_COUNT_COLUMN = "%21s %8d";
    private static final String TWO_COUNT_COLUMNS = "%21s %8d %21s %8d";
    private static final String ONE_COUNT_COLUMN_HEADER = "%21s %8s";

    // Formatting for checkin service - update version if row format changes
    private static final int ACTIVITY_THREAD_CHECKIN_VERSION = 4;

    static void printRow(PrintWriter pw, String format, Object...objs) {
        pw.println(String.format(format, objs));
    }

    public static void dumpMemInfoTable(PrintWriter pw, Debug.MemoryInfo memInfo, boolean checkin,
            boolean dumpFullInfo, boolean dumpDalvik, boolean dumpSummaryOnly,
            int pid, String processName,
            long nativeMax, long nativeAllocated, long nativeFree,
            long dalvikMax, long dalvikAllocated, long dalvikFree) {

        // For checkin, we print one long comma-separated list of values
        if (checkin) {
            // NOTE: if you change anything significant below, also consider changing
            // ACTIVITY_THREAD_CHECKIN_VERSION.

            // Header
            pw.print(ACTIVITY_THREAD_CHECKIN_VERSION); pw.print(',');
            pw.print(pid); pw.print(',');
            pw.print(processName); pw.print(',');

            // Heap info - max
            pw.print(nativeMax); pw.print(',');
            pw.print(dalvikMax); pw.print(',');
            pw.print("N/A,");
            pw.print(nativeMax + dalvikMax); pw.print(',');

            // Heap info - allocated
            pw.print(nativeAllocated); pw.print(',');
            pw.print(dalvikAllocated); pw.print(',');
            pw.print("N/A,");
            pw.print(nativeAllocated + dalvikAllocated); pw.print(',');

            // Heap info - free
            pw.print(nativeFree); pw.print(',');
            pw.print(dalvikFree); pw.print(',');
            pw.print("N/A,");
            pw.print(nativeFree + dalvikFree); pw.print(',');

            // Heap info - proportional set size
            pw.print(memInfo.nativePss); pw.print(',');
            pw.print(memInfo.dalvikPss); pw.print(',');
            pw.print(memInfo.otherPss); pw.print(',');
            pw.print(memInfo.getTotalPss()); pw.print(',');

            // Heap info - swappable set size
            pw.print(memInfo.nativeSwappablePss); pw.print(',');
            pw.print(memInfo.dalvikSwappablePss); pw.print(',');
            pw.print(memInfo.otherSwappablePss); pw.print(',');
            pw.print(memInfo.getTotalSwappablePss()); pw.print(',');

            // Heap info - shared dirty
            pw.print(memInfo.nativeSharedDirty); pw.print(',');
            pw.print(memInfo.dalvikSharedDirty); pw.print(',');
            pw.print(memInfo.otherSharedDirty); pw.print(',');
            pw.print(memInfo.getTotalSharedDirty()); pw.print(',');

            // Heap info - shared clean
            pw.print(memInfo.nativeSharedClean); pw.print(',');
            pw.print(memInfo.dalvikSharedClean); pw.print(',');
            pw.print(memInfo.otherSharedClean); pw.print(',');
            pw.print(memInfo.getTotalSharedClean()); pw.print(',');

            // Heap info - private Dirty
            pw.print(memInfo.nativePrivateDirty); pw.print(',');
            pw.print(memInfo.dalvikPrivateDirty); pw.print(',');
            pw.print(memInfo.otherPrivateDirty); pw.print(',');
            pw.print(memInfo.getTotalPrivateDirty()); pw.print(',');

            // Heap info - private Clean
            pw.print(memInfo.nativePrivateClean); pw.print(',');
            pw.print(memInfo.dalvikPrivateClean); pw.print(',');
            pw.print(memInfo.otherPrivateClean); pw.print(',');
            pw.print(memInfo.getTotalPrivateClean()); pw.print(',');

            // Heap info - swapped out
            pw.print(memInfo.nativeSwappedOut); pw.print(',');
            pw.print(memInfo.dalvikSwappedOut); pw.print(',');
            pw.print(memInfo.otherSwappedOut); pw.print(',');
            pw.print(memInfo.getTotalSwappedOut()); pw.print(',');

            // Heap info - swapped out pss
            if (memInfo.hasSwappedOutPss) {
                pw.print(memInfo.nativeSwappedOutPss); pw.print(',');
                pw.print(memInfo.dalvikSwappedOutPss); pw.print(',');
                pw.print(memInfo.otherSwappedOutPss); pw.print(',');
                pw.print(memInfo.getTotalSwappedOutPss()); pw.print(',');
            } else {
                pw.print("N/A,");
                pw.print("N/A,");
                pw.print("N/A,");
                pw.print("N/A,");
            }

            // Heap info - other areas
            for (int i=0; i<Debug.MemoryInfo.NUM_OTHER_STATS; i++) {
                pw.print(Debug.MemoryInfo.getOtherLabel(i)); pw.print(',');
                pw.print(memInfo.getOtherPss(i)); pw.print(',');
                pw.print(memInfo.getOtherSwappablePss(i)); pw.print(',');
                pw.print(memInfo.getOtherSharedDirty(i)); pw.print(',');
                pw.print(memInfo.getOtherSharedClean(i)); pw.print(',');
                pw.print(memInfo.getOtherPrivateDirty(i)); pw.print(',');
                pw.print(memInfo.getOtherPrivateClean(i)); pw.print(',');
                pw.print(memInfo.getOtherSwappedOut(i)); pw.print(',');
                if (memInfo.hasSwappedOutPss) {
                    pw.print(memInfo.getOtherSwappedOutPss(i)); pw.print(',');
                } else {
                    pw.print("N/A,");
                }
            }
            return;
        }

        if (!dumpSummaryOnly) {
            if (dumpFullInfo) {
                printRow(pw, HEAP_FULL_COLUMN, "", "Pss", "Pss", "Shared", "Private",
                        "Shared", "Private", memInfo.hasSwappedOutPss ? "SwapPss" : "Swap",
                        "Heap", "Heap", "Heap");
                printRow(pw, HEAP_FULL_COLUMN, "", "Total", "Clean", "Dirty", "Dirty",
                        "Clean", "Clean", "Dirty",
                        "Size", "Alloc", "Free");
                printRow(pw, HEAP_FULL_COLUMN, "", "------", "------", "------", "------",
                        "------", "------", "------", "------", "------", "------");
                printRow(pw, HEAP_FULL_COLUMN, "Native Heap", memInfo.nativePss,
                        memInfo.nativeSwappablePss, memInfo.nativeSharedDirty,
                        memInfo.nativePrivateDirty, memInfo.nativeSharedClean,
                        memInfo.nativePrivateClean, memInfo.hasSwappedOutPss ?
                        memInfo.nativeSwappedOut : memInfo.nativeSwappedOutPss,
                        nativeMax, nativeAllocated, nativeFree);
                printRow(pw, HEAP_FULL_COLUMN, "Dalvik Heap", memInfo.dalvikPss,
                        memInfo.dalvikSwappablePss, memInfo.dalvikSharedDirty,
                        memInfo.dalvikPrivateDirty, memInfo.dalvikSharedClean,
                        memInfo.dalvikPrivateClean, memInfo.hasSwappedOutPss ?
                        memInfo.dalvikSwappedOut : memInfo.dalvikSwappedOutPss,
                        dalvikMax, dalvikAllocated, dalvikFree);
            } else {
                printRow(pw, HEAP_COLUMN, "", "Pss", "Private",
                        "Private", memInfo.hasSwappedOutPss ? "SwapPss" : "Swap",
                        "Heap", "Heap", "Heap");
                printRow(pw, HEAP_COLUMN, "", "Total", "Dirty",
                        "Clean", "Dirty", "Size", "Alloc", "Free");
                printRow(pw, HEAP_COLUMN, "", "------", "------", "------",
                        "------", "------", "------", "------", "------");
                printRow(pw, HEAP_COLUMN, "Native Heap", memInfo.nativePss,
                        memInfo.nativePrivateDirty,
                        memInfo.nativePrivateClean,
                        memInfo.hasSwappedOutPss ? memInfo.nativeSwappedOutPss :
                        memInfo.nativeSwappedOut,
                        nativeMax, nativeAllocated, nativeFree);
                printRow(pw, HEAP_COLUMN, "Dalvik Heap", memInfo.dalvikPss,
                        memInfo.dalvikPrivateDirty,
                        memInfo.dalvikPrivateClean,
                        memInfo.hasSwappedOutPss ? memInfo.dalvikSwappedOutPss :
                        memInfo.dalvikSwappedOut,
                        dalvikMax, dalvikAllocated, dalvikFree);
            }

            int otherPss = memInfo.otherPss;
            int otherSwappablePss = memInfo.otherSwappablePss;
            int otherSharedDirty = memInfo.otherSharedDirty;
            int otherPrivateDirty = memInfo.otherPrivateDirty;
            int otherSharedClean = memInfo.otherSharedClean;
            int otherPrivateClean = memInfo.otherPrivateClean;
            int otherSwappedOut = memInfo.otherSwappedOut;
            int otherSwappedOutPss = memInfo.otherSwappedOutPss;

            for (int i=0; i<Debug.MemoryInfo.NUM_OTHER_STATS; i++) {
                final int myPss = memInfo.getOtherPss(i);
                final int mySwappablePss = memInfo.getOtherSwappablePss(i);
                final int mySharedDirty = memInfo.getOtherSharedDirty(i);
                final int myPrivateDirty = memInfo.getOtherPrivateDirty(i);
                final int mySharedClean = memInfo.getOtherSharedClean(i);
                final int myPrivateClean = memInfo.getOtherPrivateClean(i);
                final int mySwappedOut = memInfo.getOtherSwappedOut(i);
                final int mySwappedOutPss = memInfo.getOtherSwappedOutPss(i);
                if (myPss != 0 || mySharedDirty != 0 || myPrivateDirty != 0
                        || mySharedClean != 0 || myPrivateClean != 0
                        || (memInfo.hasSwappedOutPss ? mySwappedOutPss : mySwappedOut) != 0) {
                    if (dumpFullInfo) {
                        printRow(pw, HEAP_FULL_COLUMN, Debug.MemoryInfo.getOtherLabel(i),
                                myPss, mySwappablePss, mySharedDirty, myPrivateDirty,
                                mySharedClean, myPrivateClean,
                                memInfo.hasSwappedOutPss ? mySwappedOutPss : mySwappedOut,
                                "", "", "");
                    } else {
                        printRow(pw, HEAP_COLUMN, Debug.MemoryInfo.getOtherLabel(i),
                                myPss, myPrivateDirty,
                                myPrivateClean,
                                memInfo.hasSwappedOutPss ? mySwappedOutPss : mySwappedOut,
                                "", "", "");
                    }
                    otherPss -= myPss;
                    otherSwappablePss -= mySwappablePss;
                    otherSharedDirty -= mySharedDirty;
                    otherPrivateDirty -= myPrivateDirty;
                    otherSharedClean -= mySharedClean;
                    otherPrivateClean -= myPrivateClean;
                    otherSwappedOut -= mySwappedOut;
                    otherSwappedOutPss -= mySwappedOutPss;
                }
            }

            if (dumpFullInfo) {
                printRow(pw, HEAP_FULL_COLUMN, "Unknown", otherPss, otherSwappablePss,
                        otherSharedDirty, otherPrivateDirty, otherSharedClean, otherPrivateClean,
                        memInfo.hasSwappedOutPss ? otherSwappedOutPss : otherSwappedOut,
                        "", "", "");
                printRow(pw, HEAP_FULL_COLUMN, "TOTAL", memInfo.getTotalPss(),
                        memInfo.getTotalSwappablePss(),
                        memInfo.getTotalSharedDirty(), memInfo.getTotalPrivateDirty(),
                        memInfo.getTotalSharedClean(), memInfo.getTotalPrivateClean(),
                        memInfo.hasSwappedOutPss ? memInfo.getTotalSwappedOutPss() :
                        memInfo.getTotalSwappedOut(),
                        nativeMax+dalvikMax, nativeAllocated+dalvikAllocated,
                        nativeFree+dalvikFree);
            } else {
                printRow(pw, HEAP_COLUMN, "Unknown", otherPss,
                        otherPrivateDirty, otherPrivateClean,
                        memInfo.hasSwappedOutPss ? otherSwappedOutPss : otherSwappedOut,
                        "", "", "");
                printRow(pw, HEAP_COLUMN, "TOTAL", memInfo.getTotalPss(),
                        memInfo.getTotalPrivateDirty(),
                        memInfo.getTotalPrivateClean(),
                        memInfo.hasSwappedOutPss ? memInfo.getTotalSwappedOutPss() :
                        memInfo.getTotalSwappedOut(),
                        nativeMax+dalvikMax,
                        nativeAllocated+dalvikAllocated, nativeFree+dalvikFree);
            }

            if (dumpDalvik) {
                pw.println(" ");
                pw.println(" Dalvik Details");

                for (int i=Debug.MemoryInfo.NUM_OTHER_STATS;
                     i<Debug.MemoryInfo.NUM_OTHER_STATS + Debug.MemoryInfo.NUM_DVK_STATS; i++) {
                    final int myPss = memInfo.getOtherPss(i);
                    final int mySwappablePss = memInfo.getOtherSwappablePss(i);
                    final int mySharedDirty = memInfo.getOtherSharedDirty(i);
                    final int myPrivateDirty = memInfo.getOtherPrivateDirty(i);
                    final int mySharedClean = memInfo.getOtherSharedClean(i);
                    final int myPrivateClean = memInfo.getOtherPrivateClean(i);
                    final int mySwappedOut = memInfo.getOtherSwappedOut(i);
                    final int mySwappedOutPss = memInfo.getOtherSwappedOutPss(i);
                    if (myPss != 0 || mySharedDirty != 0 || myPrivateDirty != 0
                            || mySharedClean != 0 || myPrivateClean != 0
                            || (memInfo.hasSwappedOutPss ? mySwappedOutPss : mySwappedOut) != 0) {
                        if (dumpFullInfo) {
                            printRow(pw, HEAP_FULL_COLUMN, Debug.MemoryInfo.getOtherLabel(i),
                                    myPss, mySwappablePss, mySharedDirty, myPrivateDirty,
                                    mySharedClean, myPrivateClean,
                                    memInfo.hasSwappedOutPss ? mySwappedOutPss : mySwappedOut,
                                    "", "", "");
                        } else {
                            printRow(pw, HEAP_COLUMN, Debug.MemoryInfo.getOtherLabel(i),
                                    myPss, myPrivateDirty,
                                    myPrivateClean,
                                    memInfo.hasSwappedOutPss ? mySwappedOutPss : mySwappedOut,
                                    "", "", "");
                        }
                    }
                }
            }
        }

        pw.println(" ");
        pw.println(" App Summary");
        printRow(pw, ONE_COUNT_COLUMN_HEADER, "", "Pss(KB)");
        printRow(pw, ONE_COUNT_COLUMN_HEADER, "", "------");
        printRow(pw, ONE_COUNT_COLUMN,
            "Java Heap:", memInfo.getSummaryJavaHeap());
        printRow(pw, ONE_COUNT_COLUMN,
            "Native Heap:", memInfo.getSummaryNativeHeap());
        printRow(pw, ONE_COUNT_COLUMN,
            "Code:", memInfo.getSummaryCode());
        printRow(pw, ONE_COUNT_COLUMN,
            "Stack:", memInfo.getSummaryStack());
        printRow(pw, ONE_COUNT_COLUMN,
            "Graphics:", memInfo.getSummaryGraphics());
        printRow(pw, ONE_COUNT_COLUMN,
            "Private Other:", memInfo.getSummaryPrivateOther());
        printRow(pw, ONE_COUNT_COLUMN,
            "System:", memInfo.getSummarySystem());
        pw.println(" ");
        if (memInfo.hasSwappedOutPss) {
            printRow(pw, TWO_COUNT_COLUMNS,
                "TOTAL:", memInfo.getSummaryTotalPss(),
                "TOTAL SWAP PSS:", memInfo.getSummaryTotalSwapPss());
        } else {
            printRow(pw, TWO_COUNT_COLUMNS,
                "TOTAL:", memInfo.getSummaryTotalPss(),
                "TOTAL SWAP (KB):", memInfo.getSummaryTotalSwap());
        }
    }

    public void registerOnActivityPausedListener(Activity activity,
            OnActivityPausedListener listener) {
        synchronized (mOnPauseListeners) {
            ArrayList<OnActivityPausedListener> list = mOnPauseListeners.get(activity);
            if (list == null) {
                list = new ArrayList<OnActivityPausedListener>();
                mOnPauseListeners.put(activity, list);
            }
            list.add(listener);
        }
    }

    public void unregisterOnActivityPausedListener(Activity activity,
            OnActivityPausedListener listener) {
        synchronized (mOnPauseListeners) {
            ArrayList<OnActivityPausedListener> list = mOnPauseListeners.get(activity);
            if (list != null) {
                list.remove(listener);
            }
        }
    }

    public final ActivityInfo resolveActivityInfo(Intent intent) {
        ActivityInfo aInfo = intent.resolveActivityInfo(
                mInitialApplication.getPackageManager(), PackageManager.GET_SHARED_LIBRARY_FILES);
        if (aInfo == null) {
            // Throw an exception.
            Instrumentation.checkStartActivityResult(
                    ActivityManager.START_CLASS_NOT_FOUND, intent);
        }
        return aInfo;
    }

    public final Activity startActivityNow(Activity parent, String id,
        Intent intent, ActivityInfo activityInfo, IBinder token, Bundle state,
        Activity.NonConfigurationInstances lastNonConfigurationInstances) {
        ActivityClientRecord r = new ActivityClientRecord();
            r.token = token;
            r.ident = 0;
            r.intent = intent;
            r.state = state;
            r.parent = parent;
            r.embeddedID = id;
            r.activityInfo = activityInfo;
            r.lastNonConfigurationInstances = lastNonConfigurationInstances;
        if (localLOGV) {
            ComponentName compname = intent.getComponent();
            String name;
            if (compname != null) {
                name = compname.toShortString();
            } else {
                name = "(Intent " + intent + ").getComponent() returned null";
            }
            Slog.v(TAG, "Performing launch: action=" + intent.getAction()
                    + ", comp=" + name
                    + ", token=" + token);
        }
        return performLaunchActivity(r, null);
    }

    public final Activity getActivity(IBinder token) {
        return mActivities.get(token).activity;
    }

    public final void sendActivityResult(
            IBinder token, String id, int requestCode,
            int resultCode, Intent data) {
        if (DEBUG_RESULTS) Slog.v(TAG, "sendActivityResult: id=" + id
                + " req=" + requestCode + " res=" + resultCode + " data=" + data);
        ArrayList<ResultInfo> list = new ArrayList<ResultInfo>();
        list.add(new ResultInfo(id, requestCode, resultCode, data));
        mAppThread.scheduleSendResult(token, list);
    }

    private void sendMessage(int what, Object obj) {
        sendMessage(what, obj, 0, 0, false);
    }

    private void sendMessage(int what, Object obj, int arg1) {
        sendMessage(what, obj, arg1, 0, false);
    }

    private void sendMessage(int what, Object obj, int arg1, int arg2) {
        sendMessage(what, obj, arg1, arg2, false);
    }

    private void sendMessage(int what, Object obj, int arg1, int arg2, boolean async) {
        if (DEBUG_MESSAGES) Slog.v(
            TAG, "SCHEDULE " + what + " " + mH.codeToString(what)
            + ": " + arg1 + " / " + obj);
        Message msg = Message.obtain();
        msg.what = what;
        msg.obj = obj;
        msg.arg1 = arg1;
        msg.arg2 = arg2;
        if (async) {
            msg.setAsynchronous(true);
        }
        mH.sendMessage(msg);
    }

    private void sendMessage(int what, Object obj, int arg1, int arg2, int seq) {
        if (DEBUG_MESSAGES) Slog.v(
                TAG, "SCHEDULE " + mH.codeToString(what) + " arg1=" + arg1 + " arg2=" + arg2 +
                        "seq= " + seq);
        Message msg = Message.obtain();
        msg.what = what;
        SomeArgs args = SomeArgs.obtain();
        args.arg1 = obj;
        args.argi1 = arg1;
        args.argi2 = arg2;
        args.argi3 = seq;
        msg.obj = args;
        mH.sendMessage(msg);
    }

    final void scheduleContextCleanup(ContextImpl context, String who,
            String what) {
        ContextCleanupInfo cci = new ContextCleanupInfo();
        cci.context = context;
        cci.who = who;
        cci.what = what;
        sendMessage(H.CLEAN_UP_CONTEXT, cci);
    }

    private Activity performLaunchActivity(ActivityClientRecord r, Intent customIntent) {
        // System.out.println("##### [" + System.currentTimeMillis() + "] ActivityThread.performLaunchActivity(" + r + ")");

        ActivityInfo aInfo = r.activityInfo;
        if (r.packageInfo == null) {
            r.packageInfo = getPackageInfo(aInfo.applicationInfo, r.compatInfo,
                    Context.CONTEXT_INCLUDE_CODE);
        }

        ComponentName component = r.intent.getComponent();
        if (component == null) {
            component = r.intent.resolveActivity(
                mInitialApplication.getPackageManager());
            r.intent.setComponent(component);
        }

        if (r.activityInfo.targetActivity != null) {
            component = new ComponentName(r.activityInfo.packageName,
                    r.activityInfo.targetActivity);
        }

        Activity activity = null;
        try {
            java.lang.ClassLoader cl = r.packageInfo.getClassLoader();
            activity = mInstrumentation.newActivity(
                    cl, component.getClassName(), r.intent);
            StrictMode.incrementExpectedActivityCount(activity.getClass());
            r.intent.setExtrasClassLoader(cl);
            r.intent.prepareToEnterProcess();
            if (r.state != null) {
                r.state.setClassLoader(cl);
            }
        } catch (Exception e) {
            if (!mInstrumentation.onException(activity, e)) {
                throw new RuntimeException(
                    "Unable to instantiate activity " + component
                    + ": " + e.toString(), e);
            }
        }

        try {
            Application app = r.packageInfo.makeApplication(false, mInstrumentation);

            if (localLOGV) Slog.v(TAG, "Performing launch of " + r);
            if (localLOGV) Slog.v(
                    TAG, r + ": app=" + app
                    + ", appName=" + app.getPackageName()
                    + ", pkg=" + r.packageInfo.getPackageName()
                    + ", comp=" + r.intent.getComponent().toShortString()
                    + ", dir=" + r.packageInfo.getAppDir());

            if (activity != null) {
                Context appContext = createBaseContextForActivity(r, activity);
                CharSequence title = r.activityInfo.loadLabel(appContext.getPackageManager());
                Configuration config = new Configuration(mCompatConfiguration);
                if (r.overrideConfig != null) {
                    config.updateFrom(r.overrideConfig);
                }
                if (DEBUG_CONFIGURATION) Slog.v(TAG, "Launching activity "
                        + r.activityInfo.name + " with config " + config);
                Window window = null;
                if (r.mPendingRemoveWindow != null && r.mPreserveWindow) {
                    window = r.mPendingRemoveWindow;
                    r.mPendingRemoveWindow = null;
                    r.mPendingRemoveWindowManager = null;
                }
                activity.attach(appContext, this, getInstrumentation(), r.token,
                        r.ident, app, r.intent, r.activityInfo, title, r.parent,
                        r.embeddedID, r.lastNonConfigurationInstances, config,
                        r.referrer, r.voiceInteractor, window);

                if (customIntent != null) {
                    activity.mIntent = customIntent;
                }
                r.lastNonConfigurationInstances = null;
                activity.mStartedActivity = false;
                int theme = r.activityInfo.getThemeResource();
                if (theme != 0) {
                    activity.setTheme(theme);
                }

                activity.mCalled = false;
                if (r.isPersistable()) {
                    mInstrumentation.callActivityOnCreate(activity, r.state, r.persistentState);
                } else {
                    mInstrumentation.callActivityOnCreate(activity, r.state);
                }
                if (!activity.mCalled) {
                    throw new SuperNotCalledException(
                        "Activity " + r.intent.getComponent().toShortString() +
                        " did not call through to super.onCreate()");
                }
                r.activity = activity;
                r.stopped = true;
                if (!r.activity.mFinished) {
                    activity.performStart();
                    r.stopped = false;
                }
                if (!r.activity.mFinished) {
                    if (r.isPersistable()) {
                        if (r.state != null || r.persistentState != null) {
                            mInstrumentation.callActivityOnRestoreInstanceState(activity, r.state,
                                    r.persistentState);
                        }
                    } else if (r.state != null) {
                        mInstrumentation.callActivityOnRestoreInstanceState(activity, r.state);
                    }
                }
                if (!r.activity.mFinished) {
                    activity.mCalled = false;
                    if (r.isPersistable()) {
                        mInstrumentation.callActivityOnPostCreate(activity, r.state,
                                r.persistentState);
                    } else {
                        mInstrumentation.callActivityOnPostCreate(activity, r.state);
                    }
                    if (!activity.mCalled) {
                        throw new SuperNotCalledException(
                            "Activity " + r.intent.getComponent().toShortString() +
                            " did not call through to super.onPostCreate()");
                    }
                }
            }
            r.paused = true;

            mActivities.put(r.token, r);

        } catch (SuperNotCalledException e) {
            throw e;

        } catch (Exception e) {
            if (!mInstrumentation.onException(activity, e)) {
                throw new RuntimeException(
                    "Unable to start activity " + component
                    + ": " + e.toString(), e);
            }
        }

        return activity;
    }

    private Context createBaseContextForActivity(ActivityClientRecord r, final Activity activity) {
        int displayId = Display.DEFAULT_DISPLAY;
        try {
            displayId = ActivityManagerNative.getDefault().getActivityDisplayId(r.token);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }

        ContextImpl appContext = ContextImpl.createActivityContext(
                this, r.packageInfo, r.token, displayId, r.overrideConfig);
        appContext.setOuterContext(activity);
        Context baseContext = appContext;

        final DisplayManagerGlobal dm = DisplayManagerGlobal.getInstance();
        // For debugging purposes, if the activity's package name contains the value of
        // the "debug.use-second-display" system property as a substring, then show
        // its content on a secondary display if there is one.
        String pkgName = SystemProperties.get("debug.second-display.pkg");
        if (pkgName != null && !pkgName.isEmpty()
                && r.packageInfo.mPackageName.contains(pkgName)) {
            for (int id : dm.getDisplayIds()) {
                if (id != Display.DEFAULT_DISPLAY) {
                    Display display =
                            dm.getCompatibleDisplay(id, appContext.getDisplayAdjustments(id));
                    baseContext = appContext.createDisplayContext(display);
                    break;
                }
            }
        }
        return baseContext;
    }

    private void handleLaunchActivity(ActivityClientRecord r, Intent customIntent, String reason) {
        // can not launch the activity that its taskId or stackId is invalid.
        if (!r.isInStack()) {
            Log.w(TAG,"handleLaunchActivity stack or task is invalid, can not launch it, r:" + r);
            return;
        }

        // If we are getting ready to gc after going to the background, well
        // we are back active so skip it.
        unscheduleGcIdler();
        mSomeActivitiesChanged = true;

        if (r.profilerInfo != null) {
            mProfiler.setProfiler(r.profilerInfo);
            mProfiler.startProfiling();
        }

        // Make sure we are running with the most recent config.
        handleConfigurationChanged(null, null);

        if (localLOGV) Slog.v(
            TAG, "Handling launch of " + r);

        // Initialize before creating the activity
        WindowManagerGlobal.initialize();

        Activity a = performLaunchActivity(r, customIntent);

        if (a != null) {
            r.createdConfig = new Configuration(mConfiguration);
            reportSizeConfigurations(r);
            Bundle oldState = r.state;
            handleResumeActivity(r.token, false, r.isForward,
                    !r.activity.mFinished && !r.startsNotResumed, r.lastProcessedSeq, reason);

            if (!r.activity.mFinished && r.startsNotResumed) {
                // The activity manager actually wants this one to start out paused, because it
                // needs to be visible but isn't in the foreground. We accomplish this by going
                // through the normal startup (because activities expect to go through onResume()
                // the first time they run, before their window is displayed), and then pausing it.
                // However, in this case we do -not- need to do the full pause cycle (of freezing
                // and such) because the activity manager assumes it can just retain the current
                // state it has.
                performPauseActivityIfNeeded(r, reason);

                // We need to keep around the original state, in case we need to be created again.
                // But we only do this for pre-Honeycomb apps, which always save their state when
                // pausing, so we can not have them save their state when restarting from a paused
                // state. For HC and later, we want to (and can) let the state be saved as the
                // normal part of stopping the activity.
                if (r.isPreHoneycomb()) {
                    r.state = oldState;
                }
            }
        } else {
            // If there was an error, for any reason, tell the activity manager to stop us.
            try {
                ActivityManagerNative.getDefault()
                    .finishActivity(r.token, Activity.RESULT_CANCELED, null,
                            Activity.DONT_FINISH_TASK_WITH_ACTIVITY);
            } catch (RemoteException ex) {
                throw ex.rethrowFromSystemServer();
            }
        }
    }

    private void reportSizeConfigurations(ActivityClientRecord r) {
        Configuration[] configurations = r.activity.getResources().getSizeConfigurations();
        if (configurations == null) {
            return;
        }
        SparseIntArray horizontal = new SparseIntArray();
        SparseIntArray vertical = new SparseIntArray();
        SparseIntArray smallest = new SparseIntArray();
        for (int i = configurations.length - 1; i >= 0; i--) {
            Configuration config = configurations[i];
            if (config.screenHeightDp != Configuration.SCREEN_HEIGHT_DP_UNDEFINED) {
                vertical.put(config.screenHeightDp, 0);
            }
            if (config.screenWidthDp != Configuration.SCREEN_WIDTH_DP_UNDEFINED) {
                horizontal.put(config.screenWidthDp, 0);
            }
            if (config.smallestScreenWidthDp != Configuration.SMALLEST_SCREEN_WIDTH_DP_UNDEFINED) {
                smallest.put(config.smallestScreenWidthDp, 0);
            }
        }
        try {
            ActivityManagerNative.getDefault().reportSizeConfigurations(r.token,
                    horizontal.copyKeys(), vertical.copyKeys(), smallest.copyKeys());
        } catch (RemoteException ex) {
            throw ex.rethrowFromSystemServer();
        }

    }

    private void deliverNewIntents(ActivityClientRecord r, List<ReferrerIntent> intents) {
        final int N = intents.size();
        for (int i=0; i<N; i++) {
            ReferrerIntent intent = intents.get(i);
            intent.setExtrasClassLoader(r.activity.getClassLoader());
            intent.prepareToEnterProcess();
            r.activity.mFragments.noteStateNotSaved();
            mInstrumentation.callActivityOnNewIntent(r.activity, intent);
        }
    }

    void performNewIntents(IBinder token, List<ReferrerIntent> intents, boolean andPause) {
        final ActivityClientRecord r = mActivities.get(token);
        if (r == null) {
            return;
        }

        final boolean resumed = !r.paused;
        if (resumed) {
            r.activity.mTemporaryPause = true;
            mInstrumentation.callActivityOnPause(r.activity);
        }
        deliverNewIntents(r, intents);
        if (resumed) {
            r.activity.performResume();
            r.activity.mTemporaryPause = false;
        }

        if (r.paused && andPause) {
            // In this case the activity was in the paused state when we delivered the intent,
            // to guarantee onResume gets called after onNewIntent we temporarily resume the
            // activity and pause again as the caller wanted.
            performResumeActivity(token, false, "performNewIntents");
            performPauseActivityIfNeeded(r, "performNewIntents");
        }
    }

    private void handleNewIntent(NewIntentData data) {
        performNewIntents(data.token, data.intents, data.andPause);
    }

    public void handleRequestAssistContextExtras(RequestAssistContextExtras cmd) {
        if (mLastSessionId != cmd.sessionId) {
            // Clear the existing structures
            mLastSessionId = cmd.sessionId;
            for (int i = mLastAssistStructures.size() - 1; i >= 0; i--) {
                AssistStructure structure = mLastAssistStructures.get(i).get();
                if (structure != null) {
                    structure.clearSendChannel();
                }
                mLastAssistStructures.remove(i);
            }
        }
        Bundle data = new Bundle();
        AssistStructure structure = null;
        AssistContent content = new AssistContent();
        ActivityClientRecord r = mActivities.get(cmd.activityToken);
        Uri referrer = null;
        if (r != null) {
            r.activity.getApplication().dispatchOnProvideAssistData(r.activity, data);
            r.activity.onProvideAssistData(data);
            referrer = r.activity.onProvideReferrer();
            if (cmd.requestType == ActivityManager.ASSIST_CONTEXT_FULL) {
                structure = new AssistStructure(r.activity);
                Intent activityIntent = r.activity.getIntent();
                if (activityIntent != null && (r.window == null ||
                        (r.window.getAttributes().flags
                                & WindowManager.LayoutParams.FLAG_SECURE) == 0)) {
                    Intent intent = new Intent(activityIntent);
                    intent.setFlags(intent.getFlags() & ~(Intent.FLAG_GRANT_WRITE_URI_PERMISSION
                            | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION));
                    intent.removeUnsafeExtras();
                    content.setDefaultIntent(intent);
                } else {
                    content.setDefaultIntent(new Intent());
                }
                r.activity.onProvideAssistContent(content);
            }
        }
        if (structure == null) {
            structure = new AssistStructure();
        }
        mLastAssistStructures.add(new WeakReference<>(structure));
        IActivityManager mgr = ActivityManagerNative.getDefault();
        try {
            mgr.reportAssistContextExtras(cmd.requestToken, data, structure, content, referrer);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    public void handleTranslucentConversionComplete(IBinder token, boolean drawComplete) {
        ActivityClientRecord r = mActivities.get(token);
        if (r != null) {
            r.activity.onTranslucentConversionComplete(drawComplete);
        }
    }

    public void onNewActivityOptions(IBinder token, ActivityOptions options) {
        ActivityClientRecord r = mActivities.get(token);
        if (r != null) {
            r.activity.onNewActivityOptions(options);
        }
    }

    public void handleCancelVisibleBehind(IBinder token) {
        ActivityClientRecord r = mActivities.get(token);
        if (r != null) {
            mSomeActivitiesChanged = true;
            final Activity activity = r.activity;
            if (activity.mVisibleBehind) {
                activity.mCalled = false;
                activity.onVisibleBehindCanceled();
                // Tick, tick, tick. The activity has 500 msec to return or it will be destroyed.
                if (!activity.mCalled) {
                    throw new SuperNotCalledException("Activity " + activity.getLocalClassName() +
                            " did not call through to super.onVisibleBehindCanceled()");
                }
                activity.mVisibleBehind = false;
            }
        }
        try {
            ActivityManagerNative.getDefault().backgroundResourcesReleased(token);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    public void handleOnBackgroundVisibleBehindChanged(IBinder token, boolean visible) {
        ActivityClientRecord r = mActivities.get(token);
        if (r != null) {
            r.activity.onBackgroundVisibleBehindChanged(visible);
        }
    }

    public void handleInstallProvider(ProviderInfo info) {
        final StrictMode.ThreadPolicy oldPolicy = StrictMode.allowThreadDiskWrites();
        try {
            installContentProviders(mInitialApplication, Lists.newArrayList(info));
        } finally {
            StrictMode.setThreadPolicy(oldPolicy);
        }
    }

    private void handleEnterAnimationComplete(IBinder token) {
        ActivityClientRecord r = mActivities.get(token);
        if (r != null) {
            r.activity.dispatchEnterAnimationComplete();
        }
    }

    private void handleStartBinderTracking() {
        Binder.enableTracing();
    }

    private void handleStopBinderTrackingAndDump(ParcelFileDescriptor fd) {
        try {
            Binder.disableTracing();
            Binder.getTransactionTracker().writeTracesToFile(fd);
        } finally {
            IoUtils.closeQuietly(fd);
            Binder.getTransactionTracker().clearTraces();
        }
    }

    private void handleMultiWindowModeChanged(IBinder token, boolean isInMultiWindowMode) {
        final ActivityClientRecord r = mActivities.get(token);
        if (r != null) {
            r.activity.dispatchMultiWindowModeChanged(isInMultiWindowMode);
        }
    }

    private void handlePictureInPictureModeChanged(IBinder token, boolean isInPipMode) {
        final ActivityClientRecord r = mActivities.get(token);
        if (r != null) {
            r.activity.dispatchPictureInPictureModeChanged(isInPipMode);
        }
    }

    private void handleLocalVoiceInteractionStarted(IBinder token, IVoiceInteractor interactor) {
        final ActivityClientRecord r = mActivities.get(token);
        if (r != null) {
            r.voiceInteractor = interactor;
            r.activity.setVoiceInteractor(interactor);
            if (interactor == null) {
                r.activity.onLocalVoiceInteractionStopped();
            } else {
                r.activity.onLocalVoiceInteractionStarted();
            }
        }
    }

    private static final ThreadLocal<Intent> sCurrentBroadcastIntent = new ThreadLocal<Intent>();

    /**
     * Return the Intent that's currently being handled by a
     * BroadcastReceiver on this thread, or null if none.
     * @hide
     */
    public static Intent getIntentBeingBroadcast() {
        return sCurrentBroadcastIntent.get();
    }

    private void handleReceiver(ReceiverData data) {
        // If we are getting ready to gc after going to the background, well
        // we are back active so skip it.
        unscheduleGcIdler();

        String component = data.intent.getComponent().getClassName();

        LoadedApk packageInfo = getPackageInfoNoCheck(
                data.info.applicationInfo, data.compatInfo);

        IActivityManager mgr = ActivityManagerNative.getDefault();

        BroadcastReceiver receiver;
        try {
            java.lang.ClassLoader cl = packageInfo.getClassLoader();
            data.intent.setExtrasClassLoader(cl);
            data.intent.prepareToEnterProcess();
            data.setExtrasClassLoader(cl);
            receiver = (BroadcastReceiver)cl.loadClass(component).newInstance();
        } catch (Exception e) {
            if (DEBUG_BROADCAST) Slog.i(TAG,
                    "Finishing failed broadcast to " + data.intent.getComponent());
            data.sendFinished(mgr);
            throw new RuntimeException(
                "Unable to instantiate receiver " + component
                + ": " + e.toString(), e);
        }

        try {
            Application app = packageInfo.makeApplication(false, mInstrumentation);

            if (localLOGV) Slog.v(
                TAG, "Performing receive of " + data.intent
                + ": app=" + app
                + ", appName=" + app.getPackageName()
                + ", pkg=" + packageInfo.getPackageName()
                + ", comp=" + data.intent.getComponent().toShortString()
                + ", dir=" + packageInfo.getAppDir());

            ContextImpl context = (ContextImpl)app.getBaseContext();
            sCurrentBroadcastIntent.set(data.intent);
            receiver.setPendingResult(data);
            receiver.onReceive(context.getReceiverRestrictedContext(),
                    data.intent);
        } catch (Exception e) {
            if (DEBUG_BROADCAST) Slog.i(TAG,
                    "Finishing failed broadcast to " + data.intent.getComponent());
            data.sendFinished(mgr);
            if (!mInstrumentation.onException(receiver, e)) {
                throw new RuntimeException(
                    "Unable to start receiver " + component
                    + ": " + e.toString(), e);
            }
        } finally {
            sCurrentBroadcastIntent.set(null);
        }

        if (receiver.getPendingResult() != null) {
            data.finish();
        }
    }

    // Instantiate a BackupAgent and tell it that it's alive
    private void handleCreateBackupAgent(CreateBackupAgentData data) {
        if (DEBUG_BACKUP) Slog.v(TAG, "handleCreateBackupAgent: " + data);

        // Sanity check the requested target package's uid against ours
        try {
            PackageInfo requestedPackage = getPackageManager().getPackageInfo(
                    data.appInfo.packageName, 0, UserHandle.myUserId());
            if (requestedPackage.applicationInfo.uid != Process.myUid()) {
                Slog.w(TAG, "Asked to instantiate non-matching package "
                        + data.appInfo.packageName);
                return;
            }
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }

        // no longer idle; we have backup work to do
        unscheduleGcIdler();

        // instantiate the BackupAgent class named in the manifest
        LoadedApk packageInfo = getPackageInfoNoCheck(data.appInfo, data.compatInfo);
        String packageName = packageInfo.mPackageName;
        if (packageName == null) {
            Slog.d(TAG, "Asked to create backup agent for nonexistent package");
            return;
        }

        String classname = data.appInfo.backupAgentName;
        // full backup operation but no app-supplied agent?  use the default implementation
        if (classname == null && (data.backupMode == IApplicationThread.BACKUP_MODE_FULL
                || data.backupMode == IApplicationThread.BACKUP_MODE_RESTORE_FULL)) {
            classname = "android.app.backup.FullBackupAgent";
        }

        try {
            IBinder binder = null;
            BackupAgent agent = mBackupAgents.get(packageName);
            if (agent != null) {
                // reusing the existing instance
                if (DEBUG_BACKUP) {
                    Slog.v(TAG, "Reusing existing agent instance");
                }
                binder = agent.onBind();
            } else {
                try {
                    if (DEBUG_BACKUP) Slog.v(TAG, "Initializing agent class " + classname);

                    java.lang.ClassLoader cl = packageInfo.getClassLoader();
                    agent = (BackupAgent) cl.loadClass(classname).newInstance();

                    // set up the agent's context
                    ContextImpl context = ContextImpl.createAppContext(this, packageInfo);
                    context.setOuterContext(agent);
                    agent.attach(context);

                    agent.onCreate();
                    binder = agent.onBind();
                    mBackupAgents.put(packageName, agent);
                } catch (Exception e) {
                    // If this is during restore, fail silently; otherwise go
                    // ahead and let the user see the crash.
                    Slog.e(TAG, "Agent threw during creation: " + e);
                    if (data.backupMode != IApplicationThread.BACKUP_MODE_RESTORE
                            && data.backupMode != IApplicationThread.BACKUP_MODE_RESTORE_FULL) {
                        throw e;
                    }
                    // falling through with 'binder' still null
                }
            }

            // tell the OS that we're live now
            try {
                ActivityManagerNative.getDefault().backupAgentCreated(packageName, binder);
            } catch (RemoteException e) {
                throw e.rethrowFromSystemServer();
            }
        } catch (Exception e) {
            throw new RuntimeException("Unable to create BackupAgent "
                    + classname + ": " + e.toString(), e);
        }
    }

    // Tear down a BackupAgent
    private void handleDestroyBackupAgent(CreateBackupAgentData data) {
        if (DEBUG_BACKUP) Slog.v(TAG, "handleDestroyBackupAgent: " + data);

        LoadedApk packageInfo = getPackageInfoNoCheck(data.appInfo, data.compatInfo);
        String packageName = packageInfo.mPackageName;
        BackupAgent agent = mBackupAgents.get(packageName);
        if (agent != null) {
            try {
                agent.onDestroy();
            } catch (Exception e) {
                Slog.w(TAG, "Exception thrown in onDestroy by backup agent of " + data.appInfo);
                e.printStackTrace();
            }
            mBackupAgents.remove(packageName);
        } else {
            Slog.w(TAG, "Attempt to destroy unknown backup agent " + data);
        }
    }

    private void handleCreateService(CreateServiceData data) {
        // If we are getting ready to gc after going to the background, well
        // we are back active so skip it.
        unscheduleGcIdler();

        LoadedApk packageInfo = getPackageInfoNoCheck(
                data.info.applicationInfo, data.compatInfo);
        Service service = null;
        try {
            java.lang.ClassLoader cl = packageInfo.getClassLoader();
            service = (Service) cl.loadClass(data.info.name).newInstance();
        } catch (Exception e) {
            if (!mInstrumentation.onException(service, e)) {
                throw new RuntimeException(
                    "Unable to instantiate service " + data.info.name
                    + ": " + e.toString(), e);
            }
        }

        try {
            if (localLOGV) Slog.v(TAG, "Creating service " + data.info.name);

            ContextImpl context = ContextImpl.createAppContext(this, packageInfo);
            context.setOuterContext(service);

            Application app = packageInfo.makeApplication(false, mInstrumentation);
            service.attach(context, this, data.info.name, data.token, app,
                    ActivityManagerNative.getDefault());
            service.onCreate();
            mServices.put(data.token, service);
            try {
                ActivityManagerNative.getDefault().serviceDoneExecuting(
                        data.token, SERVICE_DONE_EXECUTING_ANON, 0, 0);
            } catch (RemoteException e) {
                throw e.rethrowFromSystemServer();
            }
        } catch (Exception e) {
            if (!mInstrumentation.onException(service, e)) {
                throw new RuntimeException(
                    "Unable to create service " + data.info.name
                    + ": " + e.toString(), e);
            }
        }
    }

    private void handleBindService(BindServiceData data) {
        Service s = mServices.get(data.token);
        if (DEBUG_SERVICE)
            Slog.v(TAG, "handleBindService s=" + s + " rebind=" + data.rebind);
        if (s != null) {
            try {
                data.intent.setExtrasClassLoader(s.getClassLoader());
                data.intent.prepareToEnterProcess();
                try {
                    if (!data.rebind) {
                        IBinder binder = s.onBind(data.intent);
                        ActivityManagerNative.getDefault().publishService(
                                data.token, data.intent, binder);
                    } else {
                        s.onRebind(data.intent);
                        ActivityManagerNative.getDefault().serviceDoneExecuting(
                                data.token, SERVICE_DONE_EXECUTING_ANON, 0, 0);
                    }
                    ensureJitEnabled();
                } catch (RemoteException ex) {
                    throw ex.rethrowFromSystemServer();
                }
            } catch (Exception e) {
                if (!mInstrumentation.onException(s, e)) {
                    throw new RuntimeException(
                            "Unable to bind to service " + s
                            + " with " + data.intent + ": " + e.toString(), e);
                }
            }
        }
    }

    private void handleUnbindService(BindServiceData data) {
        Service s = mServices.get(data.token);
        if (s != null) {
            try {
                data.intent.setExtrasClassLoader(s.getClassLoader());
                data.intent.prepareToEnterProcess();
                boolean doRebind = s.onUnbind(data.intent);
                try {
                    if (doRebind) {
                        ActivityManagerNative.getDefault().unbindFinished(
                                data.token, data.intent, doRebind);
                    } else {
                        ActivityManagerNative.getDefault().serviceDoneExecuting(
                                data.token, SERVICE_DONE_EXECUTING_ANON, 0, 0);
                    }
                } catch (RemoteException ex) {
                    throw ex.rethrowFromSystemServer();
                }
            } catch (Exception e) {
                if (!mInstrumentation.onException(s, e)) {
                    throw new RuntimeException(
                            "Unable to unbind to service " + s
                            + " with " + data.intent + ": " + e.toString(), e);
                }
            }
        }
    }

    private void handleDumpService(DumpComponentInfo info) {
        final StrictMode.ThreadPolicy oldPolicy = StrictMode.allowThreadDiskWrites();
        try {
            Service s = mServices.get(info.token);
            if (s != null) {
                PrintWriter pw = new FastPrintWriter(new FileOutputStream(
                        info.fd.getFileDescriptor()));
                s.dump(info.fd.getFileDescriptor(), pw, info.args);
                pw.flush();
            }
        } finally {
            IoUtils.closeQuietly(info.fd);
            StrictMode.setThreadPolicy(oldPolicy);
        }
    }

    private void handleDumpActivity(DumpComponentInfo info) {
        final StrictMode.ThreadPolicy oldPolicy = StrictMode.allowThreadDiskWrites();
        try {
            ActivityClientRecord r = mActivities.get(info.token);
            if (r != null && r.activity != null) {
                PrintWriter pw = new FastPrintWriter(new FileOutputStream(
                        info.fd.getFileDescriptor()));
                r.activity.dump(info.prefix, info.fd.getFileDescriptor(), pw, info.args);
                pw.flush();
            }
        } finally {
            IoUtils.closeQuietly(info.fd);
            StrictMode.setThreadPolicy(oldPolicy);
        }
    }

    private void handleDumpProvider(DumpComponentInfo info) {
        final StrictMode.ThreadPolicy oldPolicy = StrictMode.allowThreadDiskWrites();
        try {
            ProviderClientRecord r = mLocalProviders.get(info.token);
            if (r != null && r.mLocalProvider != null) {
                PrintWriter pw = new FastPrintWriter(new FileOutputStream(
                        info.fd.getFileDescriptor()));
                r.mLocalProvider.dump(info.fd.getFileDescriptor(), pw, info.args);
                pw.flush();
            }
        } finally {
            IoUtils.closeQuietly(info.fd);
            StrictMode.setThreadPolicy(oldPolicy);
        }
    }

    private void handleServiceArgs(ServiceArgsData data) {
        Service s = mServices.get(data.token);
        if (s != null) {
            try {
                if (data.args != null) {
                    data.args.setExtrasClassLoader(s.getClassLoader());
                    data.args.prepareToEnterProcess();
                }
                int res;
                if (!data.taskRemoved) {
                    res = s.onStartCommand(data.args, data.flags, data.startId);
                } else {
                    s.onTaskRemoved(data.args);
                    res = Service.START_TASK_REMOVED_COMPLETE;
                }

                QueuedWork.waitToFinish();

                try {
                    ActivityManagerNative.getDefault().serviceDoneExecuting(
                            data.token, SERVICE_DONE_EXECUTING_START, data.startId, res);
                } catch (RemoteException e) {
                    throw e.rethrowFromSystemServer();
                }
                ensureJitEnabled();
            } catch (Exception e) {
                if (!mInstrumentation.onException(s, e)) {
                    throw new RuntimeException(
                            "Unable to start service " + s
                            + " with " + data.args + ": " + e.toString(), e);
                }
            }
        }
    }

    private void handleStopService(IBinder token) {
        Service s = mServices.remove(token);
        if (s != null) {
            try {
                if (localLOGV) Slog.v(TAG, "Destroying service " + s);
                s.onDestroy();
                Context context = s.getBaseContext();
                if (context instanceof ContextImpl) {
                    final String who = s.getClassName();
                    ((ContextImpl) context).scheduleFinalCleanup(who, "Service");
                }

                QueuedWork.waitToFinish();

                try {
                    ActivityManagerNative.getDefault().serviceDoneExecuting(
                            token, SERVICE_DONE_EXECUTING_STOP, 0, 0);
                } catch (RemoteException e) {
                    throw e.rethrowFromSystemServer();
                }
            } catch (Exception e) {
                if (!mInstrumentation.onException(s, e)) {
                    throw new RuntimeException(
                            "Unable to stop service " + s
                            + ": " + e.toString(), e);
                }
                Slog.i(TAG, "handleStopService: exception for " + token, e);
            }
        } else {
            Slog.i(TAG, "handleStopService: token=" + token + " not found.");
        }
        //Slog.i(TAG, "Running services: " + mServices);
    }

    public final ActivityClientRecord performResumeActivity(IBinder token,
            boolean clearHide, String reason) {
        ActivityClientRecord r = mActivities.get(token);
        if (localLOGV) Slog.v(TAG, "Performing resume of " + r
                + " finished=" + r.activity.mFinished);
        if (r != null && !r.activity.mFinished) {
            if (clearHide) {
                r.hideForNow = false;
                r.activity.mStartedActivity = false;
            }
            try {
                r.activity.onStateNotSaved();
                r.activity.mFragments.noteStateNotSaved();
                if (r.pendingIntents != null) {
                    deliverNewIntents(r, r.pendingIntents);
                    r.pendingIntents = null;
                }
                if (r.pendingResults != null) {
                    deliverResults(r, r.pendingResults);
                    r.pendingResults = null;
                }
                r.activity.performResume();

                // If there is a pending local relaunch that was requested when the activity was
                // paused, it will put the activity into paused state when it finally happens.
                // Since the activity resumed before being relaunched, we don't want that to happen,
                // so we need to clear the request to relaunch paused.
                for (int i = mRelaunchingActivities.size() - 1; i >= 0; i--) {
                    final ActivityClientRecord relaunching = mRelaunchingActivities.get(i);
                    if (relaunching.token == r.token
                            && relaunching.onlyLocalRequest && relaunching.startsNotResumed) {
                        relaunching.startsNotResumed = false;
                    }
                }

                EventLog.writeEvent(LOG_AM_ON_RESUME_CALLED, UserHandle.myUserId(),
                        r.activity.getComponentName().getClassName(), reason);

                r.paused = false;
                r.stopped = false;
                r.state = null;
                r.persistentState = null;
            } catch (Exception e) {
                if (!mInstrumentation.onException(r.activity, e)) {
                    throw new RuntimeException(
                        "Unable to resume activity "
                        + r.intent.getComponent().toShortString()
                        + ": " + e.toString(), e);
                }
            }
        }
        return r;
    }

    static final void cleanUpPendingRemoveWindows(ActivityClientRecord r, boolean force) {
        if (r.mPreserveWindow && !force) {
            return;
        }
        if (r.mPendingRemoveWindow != null) {
            r.mPendingRemoveWindowManager.removeViewImmediate(
                    r.mPendingRemoveWindow.getDecorView());
            IBinder wtoken = r.mPendingRemoveWindow.getDecorView().getWindowToken();
            if (wtoken != null) {
                WindowManagerGlobal.getInstance().closeAll(wtoken,
                        r.activity.getClass().getName(), "Activity");
            }
        }
        r.mPendingRemoveWindow = null;
        r.mPendingRemoveWindowManager = null;
    }

    final void handleResumeActivity(IBinder token,
            boolean clearHide, boolean isForward, boolean reallyResume, int seq, String reason) {
        ActivityClientRecord r = mActivities.get(token);
        if (!checkAndUpdateLifecycleSeq(seq, r, "resumeActivity")) {
            return;
        }

        // If we are getting ready to gc after going to the background, well
        // we are back active so skip it.
        unscheduleGcIdler();
        mSomeActivitiesChanged = true;

        // TODO Push resumeArgs into the activity for consideration
        r = performResumeActivity(token, clearHide, reason);

        if (r != null) {
            final Activity a = r.activity;

            if (localLOGV) Slog.v(
                TAG, "Resume " + r + " started activity: " +
                a.mStartedActivity + ", hideForNow: " + r.hideForNow
                + ", finished: " + a.mFinished);

            final int forwardBit = isForward ?
                    WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION : 0;

            // If the window hasn't yet been added to the window manager,
            // and this guy didn't finish itself or start another activity,
            // then go ahead and add the window.
            boolean willBeVisible = !a.mStartedActivity;
            if (!willBeVisible) {
                try {
                    willBeVisible = ActivityManagerNative.getDefault().willActivityBeVisible(
                            a.getActivityToken());
                } catch (RemoteException e) {
                    throw e.rethrowFromSystemServer();
                }
            }
            if (r.window == null && !a.mFinished && willBeVisible) {
                r.window = r.activity.getWindow();
                View decor = r.window.getDecorView();
                decor.setVisibility(View.INVISIBLE);
                ViewManager wm = a.getWindowManager();
                WindowManager.LayoutParams l = r.window.getAttributes();
                a.mDecor = decor;
                l.type = WindowManager.LayoutParams.TYPE_BASE_APPLICATION;
                l.softInputMode |= forwardBit;
                if (r.mPreserveWindow) {
                    // if the preserve decor view is not attached to window, we
                    // should make sure that it will been attached in the following
                    // workflow.
                    if(DecorView.isAddedToWindow(decor)) a.mWindowAdded = true;
                    r.mPreserveWindow = false;
                    // Normally the ViewRoot sets up callbacks with the Activity
                    // in addView->ViewRootImpl#setView. If we are instead reusing
                    // the decor view we have to notify the view root that the
                    // callbacks may have changed.
                    ViewRootImpl impl = decor.getViewRootImpl();
                    if (impl != null) {
                        impl.notifyChildRebuilt();
                    }
                }
                if (a.mVisibleFromClient && !a.mWindowAdded) {
                    a.mWindowAdded = true;
                    wm.addView(decor, l);
                    DecorView.setAddedToWindow(a.mDecor);
                }

            // If the window has already been added, but during resume
            // we started another activity, then don't yet make the
            // window visible.
            } else if (!willBeVisible) {
                if (localLOGV) Slog.v(
                    TAG, "Launch " + r + " mStartedActivity set");
                r.hideForNow = true;
            }

            // Get rid of anything left hanging around.
            cleanUpPendingRemoveWindows(r, false /* force */);

            // The window is now visible if it has been added, we are not
            // simply finishing, and we are not starting another activity.
            if (!r.activity.mFinished && willBeVisible
                    && r.activity.mDecor != null && !r.hideForNow) {
                if (r.newConfig != null) {
                    performConfigurationChangedForActivity(r, r.newConfig, REPORT_TO_ACTIVITY);
                    if (DEBUG_CONFIGURATION) Slog.v(TAG, "Resuming activity "
                            + r.activityInfo.name + " with newConfig " + r.activity.mCurrentConfig);
                    r.newConfig = null;
                }
                if (localLOGV) Slog.v(TAG, "Resuming " + r + " with isForward="
                        + isForward);
                WindowManager.LayoutParams l = r.window.getAttributes();
                if ((l.softInputMode
                        & WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION)
                        != forwardBit) {
                    l.softInputMode = (l.softInputMode
                            & (~WindowManager.LayoutParams.SOFT_INPUT_IS_FORWARD_NAVIGATION))
                            | forwardBit;
                    if (r.activity.mVisibleFromClient) {
                        ViewManager wm = a.getWindowManager();
                        View decor = r.window.getDecorView();
                        wm.updateViewLayout(decor, l);
                    }
                }
                r.activity.mVisibleFromServer = true;
                mNumVisibleActivities++;
                if (r.activity.mVisibleFromClient) {
                    r.activity.makeVisible();
                }
            }

            if (!r.onlyLocalRequest) {
                r.nextIdle = mNewActivities;
                mNewActivities = r;
                if (localLOGV) Slog.v(
                    TAG, "Scheduling idle handler for " + r);
                Looper.myQueue().addIdleHandler(new Idler());
            }
            r.onlyLocalRequest = false;

            // Tell the activity manager we have resumed.
            if (reallyResume) {
                try {
                    ActivityManagerNative.getDefault().activityResumed(token);
                } catch (RemoteException ex) {
                    throw ex.rethrowFromSystemServer();
                }
            }

        } else {
            // If an exception was thrown when trying to resume, then
            // just end this activity.
            try {
                ActivityManagerNative.getDefault()
                    .finishActivity(token, Activity.RESULT_CANCELED, null,
                            Activity.DONT_FINISH_TASK_WITH_ACTIVITY);
            } catch (RemoteException ex) {
                throw ex.rethrowFromSystemServer();
            }
        }
    }

    private int mThumbnailWidth = -1;
    private int mThumbnailHeight = -1;
    private Bitmap mAvailThumbnailBitmap = null;
    private Canvas mThumbnailCanvas = null;

    private Bitmap createThumbnailBitmap(ActivityClientRecord r) {
        Bitmap thumbnail = mAvailThumbnailBitmap;
        try {
            if (thumbnail == null) {
                int w = mThumbnailWidth;
                int h;
                if (w < 0) {
                    Resources res = r.activity.getResources();
                    int wId = com.android.internal.R.dimen.thumbnail_width;
                    int hId = com.android.internal.R.dimen.thumbnail_height;
                    mThumbnailWidth = w = res.getDimensionPixelSize(wId);
                    mThumbnailHeight = h = res.getDimensionPixelSize(hId);
                } else {
                    h = mThumbnailHeight;
                }

                // On platforms where we don't want thumbnails, set dims to (0,0)
                if ((w > 0) && (h > 0)) {
                    thumbnail = Bitmap.createBitmap(r.activity.getResources().getDisplayMetrics(),
                            w, h, THUMBNAIL_FORMAT);
                    thumbnail.eraseColor(0);
                }
            }

            if (thumbnail != null) {
                Canvas cv = mThumbnailCanvas;
                if (cv == null) {
                    mThumbnailCanvas = cv = new Canvas();
                }

                cv.setBitmap(thumbnail);
                if (!r.activity.onCreateThumbnail(thumbnail, cv)) {
                    mAvailThumbnailBitmap = thumbnail;
                    thumbnail = null;
                }
                cv.setBitmap(null);
            }

        } catch (Exception e) {
            if (!mInstrumentation.onException(r.activity, e)) {
                throw new RuntimeException(
                        "Unable to create thumbnail of "
                        + r.intent.getComponent().toShortString()
                        + ": " + e.toString(), e);
            }
            thumbnail = null;
        }

        return thumbnail;
    }

    private void handlePauseActivity(IBinder token, boolean finished,
            boolean userLeaving, int configChanges, boolean dontReport, int seq) {
        ActivityClientRecord r = mActivities.get(token);
        if (DEBUG_ORDER) Slog.d(TAG, "handlePauseActivity " + r + ", seq: " + seq);
        if (!checkAndUpdateLifecycleSeq(seq, r, "pauseActivity")) {
            return;
        }
        if (r != null) {
            //Slog.v(TAG, "userLeaving=" + userLeaving + " handling pause of " + r);
            if (userLeaving) {
                performUserLeavingActivity(r);
            }

            r.activity.mConfigChangeFlags |= configChanges;
            performPauseActivity(token, finished, r.isPreHoneycomb(), "handlePauseActivity");

            // Make sure any pending writes are now committed.
            if (r.isPreHoneycomb()) {
                QueuedWork.waitToFinish();
            }

            // Tell the activity manager we have paused.
            if (!dontReport) {
                try {
                    ActivityManagerNative.getDefault().activityPaused(token);
                } catch (RemoteException ex) {
                    throw ex.rethrowFromSystemServer();
                }
            }
            mSomeActivitiesChanged = true;
        }
    }

    final void performUserLeavingActivity(ActivityClientRecord r) {
        mInstrumentation.callActivityOnUserLeaving(r.activity);
    }

    final Bundle performPauseActivity(IBinder token, boolean finished,
            boolean saveState, String reason) {
        ActivityClientRecord r = mActivities.get(token);
        return r != null ? performPauseActivity(r, finished, saveState, reason) : null;
    }

    final Bundle performPauseActivity(ActivityClientRecord r, boolean finished,
            boolean saveState, String reason) {
        if (r.paused) {
            if (r.activity.mFinished) {
                // If we are finishing, we won't call onResume() in certain cases.
                // So here we likewise don't want to call onPause() if the activity
                // isn't resumed.
                return null;
            }
            RuntimeException e = new RuntimeException(
                    "Performing pause of activity that is not resumed: "
                    + r.intent.getComponent().toShortString());
            Slog.e(TAG, e.getMessage(), e);
        }
        if (finished) {
            r.activity.mFinished = true;
        }

        // Next have the activity save its current state and managed dialogs...
        if (!r.activity.mFinished && saveState) {
            callCallActivityOnSaveInstanceState(r);
        }

        performPauseActivityIfNeeded(r, reason);

        // Notify any outstanding on paused listeners
        ArrayList<OnActivityPausedListener> listeners;
        synchronized (mOnPauseListeners) {
            listeners = mOnPauseListeners.remove(r.activity);
        }
        int size = (listeners != null ? listeners.size() : 0);
        for (int i = 0; i < size; i++) {
            listeners.get(i).onPaused(r.activity);
        }

        return !r.activity.mFinished && saveState ? r.state : null;
    }

    private void performPauseActivityIfNeeded(ActivityClientRecord r, String reason) {
        if (r.paused) {
            // You are already paused silly...
            return;
        }

        try {
            r.activity.mCalled = false;
            mInstrumentation.callActivityOnPause(r.activity);
            EventLog.writeEvent(LOG_AM_ON_PAUSE_CALLED, UserHandle.myUserId(),
                    r.activity.getComponentName().getClassName(), reason);
            if (!r.activity.mCalled) {
                throw new SuperNotCalledException("Activity " + safeToComponentShortString(r.intent)
                        + " did not call through to super.onPause()");
            }
        } catch (SuperNotCalledException e) {
            throw e;
        } catch (Exception e) {
            if (!mInstrumentation.onException(r.activity, e)) {
                throw new RuntimeException("Unable to pause activity "
                        + safeToComponentShortString(r.intent) + ": " + e.toString(), e);
            }
        }
        r.paused = true;
    }

    final void performStopActivity(IBinder token, boolean saveState, String reason) {
        ActivityClientRecord r = mActivities.get(token);
        performStopActivityInner(r, null, false, saveState, reason);
    }

    private static class StopInfo implements Runnable {
        ActivityClientRecord activity;
        Bundle state;
        PersistableBundle persistentState;
        CharSequence description;

        @Override public void run() {
            // Tell activity manager we have been stopped.
            try {
                if (DEBUG_MEMORY_TRIM) Slog.v(TAG, "Reporting activity stopped: " + activity);
                ActivityManagerNative.getDefault().activityStopped(
                    activity.token, state, persistentState, description);
            } catch (RemoteException ex) {
                if (ex instanceof TransactionTooLargeException
                        && activity.packageInfo.getTargetSdkVersion() < Build.VERSION_CODES.N) {
                    Log.e(TAG, "App sent too much data in instance state, so it was ignored", ex);
                    return;
                }
                throw ex.rethrowFromSystemServer();
            }
        }
    }

    private static final class ProviderRefCount {
        public final IActivityManager.ContentProviderHolder holder;
        public final ProviderClientRecord client;
        public int stableCount;
        public int unstableCount;

        // When this is set, the stable and unstable ref counts are 0 and
        // we have a pending operation scheduled to remove the ref count
        // from the activity manager.  On the activity manager we are still
        // holding an unstable ref, though it is not reflected in the counts
        // here.
        public boolean removePending;

        ProviderRefCount(IActivityManager.ContentProviderHolder inHolder,
                ProviderClientRecord inClient, int sCount, int uCount) {
            holder = inHolder;
            client = inClient;
            stableCount = sCount;
            unstableCount = uCount;
        }
    }

    static final class ProviderAcquiringCount {
        public int acquiringCount;

        ProviderAcquiringCount(int aCount) {
            acquiringCount = aCount;
        }
    }

    /**
     * Core implementation of stopping an activity.  Note this is a little
     * tricky because the server's meaning of stop is slightly different
     * than our client -- for the server, stop means to save state and give
     * it the result when it is done, but the window may still be visible.
     * For the client, we want to call onStop()/onStart() to indicate when
     * the activity's UI visibility changes.
     */
    private void performStopActivityInner(ActivityClientRecord r,
            StopInfo info, boolean keepShown, boolean saveState, String reason) {
        if (localLOGV) Slog.v(TAG, "Performing stop of " + r);
        if (r != null) {
            if (!keepShown && r.stopped) {
                if (r.activity.mFinished) {
                    // If we are finishing, we won't call onResume() in certain
                    // cases.  So here we likewise don't want to call onStop()
                    // if the activity isn't resumed.
                    return;
                }
                RuntimeException e = new RuntimeException(
                        "Performing stop of activity that is already stopped: "
                        + r.intent.getComponent().toShortString());
                Slog.e(TAG, e.getMessage(), e);
                Slog.e(TAG, r.getStateString());
            }

            // One must first be paused before stopped...
            performPauseActivityIfNeeded(r, reason);

            if (info != null) {
                try {
                    // First create a thumbnail for the activity...
                    // For now, don't create the thumbnail here; we are
                    // doing that by doing a screen snapshot.
                    info.description = r.activity.onCreateDescription();
                } catch (Exception e) {
                    if (!mInstrumentation.onException(r.activity, e)) {
                        throw new RuntimeException(
                                "Unable to save state of activity "
                                + r.intent.getComponent().toShortString()
                                + ": " + e.toString(), e);
                    }
                }
            }

            // Next have the activity save its current state and managed dialogs...
            if (!r.activity.mFinished && saveState) {
                if (r.state == null) {
                    callCallActivityOnSaveInstanceState(r);
                }
            }

            if (!keepShown) {
                try {
                    // Now we are idle.
                    r.activity.performStop(false /*preserveWindow*/);
                } catch (Exception e) {
                    if (!mInstrumentation.onException(r.activity, e)) {
                        throw new RuntimeException(
                                "Unable to stop activity "
                                + r.intent.getComponent().toShortString()
                                + ": " + e.toString(), e);
                    }
                }
                r.stopped = true;
                EventLog.writeEvent(LOG_AM_ON_STOP_CALLED, UserHandle.myUserId(),
                        r.activity.getComponentName().getClassName(), reason);
            }
        }
    }

    private void updateVisibility(ActivityClientRecord r, boolean show) {
        View v = r.activity.mDecor;
        if (v != null) {
            if (show) {
                if (!r.activity.mVisibleFromServer) {
                    r.activity.mVisibleFromServer = true;
                    mNumVisibleActivities++;
                    if (r.activity.mVisibleFromClient) {
                        r.activity.makeVisible();
                    }
                }
                if (r.newConfig != null) {
                    performConfigurationChangedForActivity(r, r.newConfig, REPORT_TO_ACTIVITY);
                    if (DEBUG_CONFIGURATION) Slog.v(TAG, "Updating activity vis "
                            + r.activityInfo.name + " with new config "
                            + r.activity.mCurrentConfig);
                    r.newConfig = null;
                }
            } else {
                if (r.activity.mVisibleFromServer) {
                    r.activity.mVisibleFromServer = false;
                    mNumVisibleActivities--;
                    v.setVisibility(View.INVISIBLE);
                }
            }
        }
    }

    private void handleStopActivity(IBinder token, boolean show, int configChanges, int seq) {
        ActivityClientRecord r = mActivities.get(token);
        if (!checkAndUpdateLifecycleSeq(seq, r, "stopActivity")) {
            return;
        }
        r.activity.mConfigChangeFlags |= configChanges;

        StopInfo info = new StopInfo();
        performStopActivityInner(r, info, show, true, "handleStopActivity");

        if (localLOGV) Slog.v(
            TAG, "Finishing stop of " + r + ": show=" + show
            + " win=" + r.window);

        updateVisibility(r, show);

        // Make sure any pending writes are now committed.
        if (!r.isPreHoneycomb()) {
            QueuedWork.waitToFinish();
        }

        // Schedule the call to tell the activity manager we have
        // stopped.  We don't do this immediately, because we want to
        // have a chance for any other pending work (in particular memory
        // trim requests) to complete before you tell the activity
        // manager to proceed and allow us to go fully into the background.
        info.activity = r;
        info.state = r.state;
        info.persistentState = r.persistentState;
        mH.post(info);
        mSomeActivitiesChanged = true;
    }

    private static boolean checkAndUpdateLifecycleSeq(int seq, ActivityClientRecord r,
            String action) {
        if (r == null) {
            return true;
        }
        if (seq < r.lastProcessedSeq) {
            if (DEBUG_ORDER) Slog.d(TAG, action + " for " + r + " ignored, because seq=" + seq
                    + " < mCurrentLifecycleSeq=" + r.lastProcessedSeq);
            return false;
        }
        r.lastProcessedSeq = seq;
        return true;
    }

    final void performRestartActivity(IBinder token) {
        ActivityClientRecord r = mActivities.get(token);
        if (r.stopped) {
            r.activity.performRestart();
            r.stopped = false;
        }
    }

    private void handleWindowVisibility(IBinder token, boolean show) {
        ActivityClientRecord r = mActivities.get(token);

        if (r == null) {
            Log.w(TAG, "handleWindowVisibility: no activity for token " + token);
            return;
        }

        if (!show && !r.stopped) {
            performStopActivityInner(r, null, show, false, "handleWindowVisibility");
        } else if (show && r.stopped) {
            // If we are getting ready to gc after going to the background, well
            // we are back active so skip it.
            unscheduleGcIdler();

            r.activity.performRestart();
            r.stopped = false;
        }
        if (r.activity.mDecor != null) {
            if (false) Slog.v(
                TAG, "Handle window " + r + " visibility: " + show);
            updateVisibility(r, show);
        }
        mSomeActivitiesChanged = true;
    }

    // TODO: This method should be changed to use {@link #performStopActivityInner} to perform to
    // stop operation on the activity to reduce code duplication and the chance of fixing a bug in
    // one place and missing the other.
    private void handleSleeping(IBinder token, boolean sleeping) {
        ActivityClientRecord r = mActivities.get(token);

        if (r == null) {
            Log.w(TAG, "handleSleeping: no activity for token " + token);
            return;
        }

        if (sleeping) {
            if (!r.stopped && !r.isPreHoneycomb()) {
                if (!r.activity.mFinished && r.state == null) {
                    callCallActivityOnSaveInstanceState(r);
                }

                try {
                    // Now we are idle.
                    r.activity.performStop(false /*preserveWindow*/);
                } catch (Exception e) {
                    if (!mInstrumentation.onException(r.activity, e)) {
                        throw new RuntimeException(
                                "Unable to stop activity "
                                + r.intent.getComponent().toShortString()
                                + ": " + e.toString(), e);
                    }
                }
                r.stopped = true;
                EventLog.writeEvent(LOG_AM_ON_STOP_CALLED, UserHandle.myUserId(),
                        r.activity.getComponentName().getClassName(), "sleeping");
            }

            // Make sure any pending writes are now committed.
            if (!r.isPreHoneycomb()) {
                QueuedWork.waitToFinish();
            }

            // Tell activity manager we slept.
            try {
                ActivityManagerNative.getDefault().activitySlept(r.token);
            } catch (RemoteException ex) {
                throw ex.rethrowFromSystemServer();
            }
        } else {
            if (r.stopped && r.activity.mVisibleFromServer) {
                r.activity.performRestart();
                r.stopped = false;
            }
        }
    }

    private void handleSetCoreSettings(Bundle coreSettings) {
        synchronized (mResourcesManager) {
            mCoreSettings = coreSettings;
        }
        onCoreSettingsChange();
    }

    private void onCoreSettingsChange() {
        boolean debugViewAttributes =
                mCoreSettings.getInt(Settings.Global.DEBUG_VIEW_ATTRIBUTES, 0) != 0;
        if (debugViewAttributes != View.mDebugViewAttributes) {
            View.mDebugViewAttributes = debugViewAttributes;

            // request all activities to relaunch for the changes to take place
            for (Map.Entry<IBinder, ActivityClientRecord> entry : mActivities.entrySet()) {
                requestRelaunchActivity(entry.getKey(), null, null, 0, false, null, null, false,
                        false /* preserveWindow */);
            }
        }
    }

    private void handleUpdatePackageCompatibilityInfo(UpdateCompatibilityData data) {
        LoadedApk apk = peekPackageInfo(data.pkg, false);
        if (apk != null) {
            apk.setCompatibilityInfo(data.info);
        }
        apk = peekPackageInfo(data.pkg, true);
        if (apk != null) {
            apk.setCompatibilityInfo(data.info);
        }
        handleConfigurationChanged(mConfiguration, data.info);
        WindowManagerGlobal.getInstance().reportNewConfiguration(mConfiguration);
    }

    private void deliverResults(ActivityClientRecord r, List<ResultInfo> results) {
        final int N = results.size();
        for (int i=0; i<N; i++) {
            ResultInfo ri = results.get(i);
            try {
                if (ri.mData != null) {
                    ri.mData.setExtrasClassLoader(r.activity.getClassLoader());
                    ri.mData.prepareToEnterProcess();
                }
                if (DEBUG_RESULTS) Slog.v(TAG,
                        "Delivering result to activity " + r + " : " + ri);
                r.activity.dispatchActivityResult(ri.mResultWho,
                        ri.mRequestCode, ri.mResultCode, ri.mData);
            } catch (Exception e) {
                if (!mInstrumentation.onException(r.activity, e)) {
                    throw new RuntimeException(
                            "Failure delivering result " + ri + " to activity "
                            + r.intent.getComponent().toShortString()
                            + ": " + e.toString(), e);
                }
            }
        }
    }

    private void handleSendResult(ResultData res) {
        ActivityClientRecord r = mActivities.get(res.token);
        if (DEBUG_RESULTS) Slog.v(TAG, "Handling send result to " + r);
        if (r != null) {
            final boolean resumed = !r.paused;
            if (!r.activity.mFinished && r.activity.mDecor != null
                    && r.hideForNow && resumed) {
                // We had hidden the activity because it started another
                // one...  we have gotten a result back and we are not
                // paused, so make sure our window is visible.
                updateVisibility(r, true);
            }
            if (resumed) {
                try {
                    // Now we are idle.
                    r.activity.mCalled = false;
                    r.activity.mTemporaryPause = true;
                    mInstrumentation.callActivityOnPause(r.activity);
                    if (!r.activity.mCalled) {
                        throw new SuperNotCalledException(
                            "Activity " + r.intent.getComponent().toShortString()
                            + " did not call through to super.onPause()");
                    }
                } catch (SuperNotCalledException e) {
                    throw e;
                } catch (Exception e) {
                    if (!mInstrumentation.onException(r.activity, e)) {
                        throw new RuntimeException(
                                "Unable to pause activity "
                                + r.intent.getComponent().toShortString()
                                + ": " + e.toString(), e);
                    }
                }
            }
            deliverResults(r, res.results);
            if (resumed) {
                r.activity.performResume();
                r.activity.mTemporaryPause = false;
            }
        }
    }

    public final ActivityClientRecord performDestroyActivity(IBinder token, boolean finishing) {
        return performDestroyActivity(token, finishing, 0, false);
    }

    private ActivityClientRecord performDestroyActivity(IBinder token, boolean finishing,
            int configChanges, boolean getNonConfigInstance) {
        ActivityClientRecord r = mActivities.get(token);
        Class<? extends Activity> activityClass = null;
        if (localLOGV) Slog.v(TAG, "Performing finish of " + r);
        if (r != null) {
            activityClass = r.activity.getClass();
            r.activity.mConfigChangeFlags |= configChanges;
            if (finishing) {
                r.activity.mFinished = true;
            }

            performPauseActivityIfNeeded(r, "destroy");

            if (!r.stopped) {
                try {
                    r.activity.performStop(r.mPreserveWindow);
                } catch (SuperNotCalledException e) {
                    throw e;
                } catch (Exception e) {
                    if (!mInstrumentation.onException(r.activity, e)) {
                        throw new RuntimeException(
                                "Unable to stop activity "
                                + safeToComponentShortString(r.intent)
                                + ": " + e.toString(), e);
                    }
                }
                r.stopped = true;
                EventLog.writeEvent(LOG_AM_ON_STOP_CALLED, UserHandle.myUserId(),
                        r.activity.getComponentName().getClassName(), "destroy");
            }
            if (getNonConfigInstance) {
                try {
                    r.lastNonConfigurationInstances
                            = r.activity.retainNonConfigurationInstances();
                } catch (Exception e) {
                    if (!mInstrumentation.onException(r.activity, e)) {
                        throw new RuntimeException(
                                "Unable to retain activity "
                                + r.intent.getComponent().toShortString()
                                + ": " + e.toString(), e);
                    }
                }
            }
            try {
                r.activity.mCalled = false;
                mInstrumentation.callActivityOnDestroy(r.activity);
                if (!r.activity.mCalled) {
                    throw new SuperNotCalledException(
                        "Activity " + safeToComponentShortString(r.intent) +
                        " did not call through to super.onDestroy()");
                }
                if (r.window != null) {
                    r.window.closeAllPanels();
                }
            } catch (SuperNotCalledException e) {
                throw e;
            } catch (Exception e) {
                if (!mInstrumentation.onException(r.activity, e)) {
                    throw new RuntimeException(
                            "Unable to destroy activity " + safeToComponentShortString(r.intent)
                            + ": " + e.toString(), e);
                }
            }
        }
        mActivities.remove(token);
        StrictMode.decrementExpectedActivityCount(activityClass);
        return r;
    }

    private static String safeToComponentShortString(Intent intent) {
        ComponentName component = intent.getComponent();
        return component == null ? "[Unknown]" : component.toShortString();
    }

    private void handleDestroyActivity(IBinder token, boolean finishing,
            int configChanges, boolean getNonConfigInstance) {
        ActivityClientRecord r = performDestroyActivity(token, finishing,
                configChanges, getNonConfigInstance);
        if (r != null) {
            cleanUpPendingRemoveWindows(r, finishing);
            WindowManager wm = r.activity.getWindowManager();
            View v = r.activity.mDecor;
            if (v != null) {
                if (r.activity.mVisibleFromServer) {
                    mNumVisibleActivities--;
                }
                IBinder wtoken = v.getWindowToken();
                if (r.activity.mWindowAdded) {
                    if (r.mPreserveWindow) {
                        // Hold off on removing this until the new activity's
                        // window is being added.
                        r.mPendingRemoveWindow = r.window;
                        r.mPendingRemoveWindowManager = wm;
                        // We can only keep the part of the view hierarchy that we control,
                        // everything else must be removed, because it might not be able to
                        // behave properly when activity is relaunching.
                        r.window.clearContentView();
                    } else {
                        wm.removeViewImmediate(v);
                    }
                }
                if (wtoken != null && r.mPendingRemoveWindow == null) {
                    WindowManagerGlobal.getInstance().closeAll(wtoken,
                            r.activity.getClass().getName(), "Activity");
                } else if (r.mPendingRemoveWindow != null) {
                    // We're preserving only one window, others should be closed so app views
                    // will be detached before the final tear down. It should be done now because
                    // some components (e.g. WebView) rely on detach callbacks to perform receiver
                    // unregister and other cleanup.
                    WindowManagerGlobal.getInstance().closeAllExceptView(token, v,
                            r.activity.getClass().getName(), "Activity");
                }
                r.activity.mDecor = null;
            }
            if (r.mPendingRemoveWindow == null) {
                // If we are delaying the removal of the activity window, then
                // we can't clean up all windows here.  Note that we can't do
                // so later either, which means any windows that aren't closed
                // by the app will leak.  Well we try to warning them a lot
                // about leaking windows, because that is a bug, so if they are
                // using this recreate facility then they get to live with leaks.
                WindowManagerGlobal.getInstance().closeAll(token,
                        r.activity.getClass().getName(), "Activity");
            }

            // Mocked out contexts won't be participating in the normal
            // process lifecycle, but if we're running with a proper
            // ApplicationContext we need to have it tear down things
            // cleanly.
            Context c = r.activity.getBaseContext();
            if (c instanceof ContextImpl) {
                ((ContextImpl) c).scheduleFinalCleanup(
                        r.activity.getClass().getName(), "Activity");
            }
        }
        if (finishing) {
            try {
                ActivityManagerNative.getDefault().activityDestroyed(token);
            } catch (RemoteException ex) {
                throw ex.rethrowFromSystemServer();
            }
        }
        mSomeActivitiesChanged = true;
    }

    /**
     * @param preserveWindow Whether the activity should try to reuse the window it created,
     *                        including the decor view after the relaunch.
     */
    public final void requestRelaunchActivity(IBinder token,
            List<ResultInfo> pendingResults, List<ReferrerIntent> pendingNewIntents,
            int configChanges, boolean notResumed, Configuration config,
            Configuration overrideConfig, boolean fromServer, boolean preserveWindow) {
        ActivityClientRecord target = null;

        synchronized (mResourcesManager) {
            for (int i=0; i<mRelaunchingActivities.size(); i++) {
                ActivityClientRecord r = mRelaunchingActivities.get(i);
                if (DEBUG_ORDER) Slog.d(TAG, "requestRelaunchActivity: " + this + ", trying: " + r);
                if (r.token == token) {
                    target = r;
                    if (pendingResults != null) {
                        if (r.pendingResults != null) {
                            r.pendingResults.addAll(pendingResults);
                        } else {
                            r.pendingResults = pendingResults;
                        }
                    }
                    if (pendingNewIntents != null) {
                        if (r.pendingIntents != null) {
                            r.pendingIntents.addAll(pendingNewIntents);
                        } else {
                            r.pendingIntents = pendingNewIntents;
                        }
                    }

                    // For each relaunch request, activity manager expects an answer
                    if (!r.onlyLocalRequest && fromServer) {
                        try {
                            ActivityManagerNative.getDefault().activityRelaunched(token);
                        } catch (RemoteException e) {
                            throw e.rethrowFromSystemServer();
                        }
                    }
                    break;
                }
            }

            if (target == null) {
                if (DEBUG_ORDER) Slog.d(TAG, "requestRelaunchActivity: target is null, fromServer:"
                        + fromServer);
                target = new ActivityClientRecord();
                target.token = token;
                target.pendingResults = pendingResults;
                target.pendingIntents = pendingNewIntents;
                target.mPreserveWindow = preserveWindow;
                if (!fromServer) {
                    final ActivityClientRecord existing = mActivities.get(token);
                    if (DEBUG_ORDER) Slog.d(TAG, "requestRelaunchActivity: " + existing);
                    if (existing != null) {
                        if (DEBUG_ORDER) Slog.d(TAG, "requestRelaunchActivity: paused= "
                                + existing.paused);;
                        target.startsNotResumed = existing.paused;
                        target.overrideConfig = existing.overrideConfig;
                    }
                    target.onlyLocalRequest = true;
                }
                mRelaunchingActivities.add(target);
                sendMessage(H.RELAUNCH_ACTIVITY, target);
            }

            if (fromServer) {
                target.startsNotResumed = notResumed;
                target.onlyLocalRequest = false;
            }
            if (config != null) {
                target.createdConfig = config;
            }
            if (overrideConfig != null) {
                target.overrideConfig = overrideConfig;
            }
            target.pendingConfigChanges |= configChanges;
            target.relaunchSeq = getLifecycleSeq();
        }
        if (DEBUG_ORDER) Slog.d(TAG, "relaunchActivity " + ActivityThread.this + ", target "
                + target + " operation received seq: " + target.relaunchSeq);
    }

    private void handleRelaunchActivity(ActivityClientRecord tmp) {
        // If we are getting ready to gc after going to the background, well
        // we are back active so skip it.
        unscheduleGcIdler();
        mSomeActivitiesChanged = true;

        Configuration changedConfig = null;
        int configChanges = 0;

        // First: make sure we have the most recent configuration and most
        // recent version of the activity, or skip it if some previous call
        // had taken a more recent version.
        synchronized (mResourcesManager) {
            int N = mRelaunchingActivities.size();
            IBinder token = tmp.token;
            tmp = null;
            for (int i=0; i<N; i++) {
                ActivityClientRecord r = mRelaunchingActivities.get(i);
                if (r.token == token) {
                    tmp = r;
                    configChanges |= tmp.pendingConfigChanges;
                    mRelaunchingActivities.remove(i);
                    i--;
                    N--;
                }
            }

            if (tmp == null) {
                if (DEBUG_CONFIGURATION) Slog.v(TAG, "Abort, activity not relaunching!");
                return;
            }

            if (DEBUG_CONFIGURATION) Slog.v(TAG, "Relaunching activity "
                    + tmp.token + " with configChanges=0x"
                    + Integer.toHexString(configChanges));

            if (mPendingConfiguration != null) {
                changedConfig = mPendingConfiguration;
                mPendingConfiguration = null;
            }
        }

        if (tmp.lastProcessedSeq > tmp.relaunchSeq) {
            Slog.wtf(TAG, "For some reason target: " + tmp + " has lower sequence: "
                    + tmp.relaunchSeq + " than current sequence: " + tmp.lastProcessedSeq);
        } else {
            tmp.lastProcessedSeq = tmp.relaunchSeq;
        }
        if (tmp.createdConfig != null) {
            // If the activity manager is passing us its current config,
            // assume that is really what we want regardless of what we
            // may have pending.
            if (mConfiguration == null
                    || (tmp.createdConfig.isOtherSeqNewer(mConfiguration)
                            && mConfiguration.diff(tmp.createdConfig) != 0)) {
                if (changedConfig == null
                        || tmp.createdConfig.isOtherSeqNewer(changedConfig)) {
                    changedConfig = tmp.createdConfig;
                }
            }
        }

        if (DEBUG_CONFIGURATION) Slog.v(TAG, "Relaunching activity "
                + tmp.token + ": changedConfig=" + changedConfig);

        // If there was a pending configuration change, execute it first.
        if (changedConfig != null) {
            mCurDefaultDisplayDpi = changedConfig.densityDpi;
            updateDefaultDensity();
            handleConfigurationChanged(changedConfig, null);
        }

        ActivityClientRecord r = mActivities.get(tmp.token);
        if (DEBUG_CONFIGURATION) Slog.v(TAG, "Handling relaunch of " + r);
        if (r == null) {
            if (!tmp.onlyLocalRequest) {
                try {
                    ActivityManagerNative.getDefault().activityRelaunched(tmp.token);
                } catch (RemoteException e) {
                    throw e.rethrowFromSystemServer();
                }
            }
            return;
        }

        r.activity.mConfigChangeFlags |= configChanges;
        r.onlyLocalRequest = tmp.onlyLocalRequest;
        r.mPreserveWindow = tmp.mPreserveWindow;
        r.lastProcessedSeq = tmp.lastProcessedSeq;
        r.relaunchSeq = tmp.relaunchSeq;
        Intent currentIntent = r.activity.mIntent;

        r.activity.mChangingConfigurations = true;

        // If we are preserving the main window across relaunches we would also like to preserve
        // the children. However the client side view system does not support preserving
        // the child views so we notify the window manager to expect these windows to
        // be replaced and defer requests to destroy or hide them. This way we can achieve
        // visual continuity. It's important that we do this here prior to pause and destroy
        // as that is when we may hide or remove the child views.
        //
        // There is another scenario, if we have decided locally to relaunch the app from a
        // call to recreate, then none of the windows will be prepared for replacement or
        // preserved by the server, so we want to notify it that we are preparing to replace
        // everything
        try {
            if (r.mPreserveWindow || r.onlyLocalRequest) {
                WindowManagerGlobal.getWindowSession().prepareToReplaceWindows(
                        r.token, !r.onlyLocalRequest);
            }
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }

        // Need to ensure state is saved.
        if (!r.paused) {
            performPauseActivity(r.token, false, r.isPreHoneycomb(), "handleRelaunchActivity");
        }
        if (r.state == null && !r.stopped && !r.isPreHoneycomb()) {
            callCallActivityOnSaveInstanceState(r);
        }

        handleDestroyActivity(r.token, false, configChanges, true);

        r.activity = null;
        r.window = null;
        r.hideForNow = false;
        r.nextIdle = null;
        // Merge any pending results and pending intents; don't just replace them
        if (tmp.pendingResults != null) {
            if (r.pendingResults == null) {
                r.pendingResults = tmp.pendingResults;
            } else {
                r.pendingResults.addAll(tmp.pendingResults);
            }
        }
        if (tmp.pendingIntents != null) {
            if (r.pendingIntents == null) {
                r.pendingIntents = tmp.pendingIntents;
            } else {
                r.pendingIntents.addAll(tmp.pendingIntents);
            }
        }
        r.startsNotResumed = tmp.startsNotResumed;
        r.overrideConfig = tmp.overrideConfig;

        handleLaunchActivity(r, currentIntent, "handleRelaunchActivity");

        if (!tmp.onlyLocalRequest) {
            try {
                ActivityManagerNative.getDefault().activityRelaunched(r.token);
                if (r.window != null) {
                    r.window.reportActivityRelaunched();
                }
            } catch (RemoteException e) {
                throw e.rethrowFromSystemServer();
            }
        }
    }

    private void callCallActivityOnSaveInstanceState(ActivityClientRecord r) {
        r.state = new Bundle();
        r.state.setAllowFds(false);
        if (r.isPersistable()) {
            r.persistentState = new PersistableBundle();
            mInstrumentation.callActivityOnSaveInstanceState(r.activity, r.state,
                    r.persistentState);
        } else {
            mInstrumentation.callActivityOnSaveInstanceState(r.activity, r.state);
        }
    }

    ArrayList<ComponentCallbacks2> collectComponentCallbacks(
            boolean allActivities, Configuration newConfig) {
        ArrayList<ComponentCallbacks2> callbacks
                = new ArrayList<ComponentCallbacks2>();

        synchronized (mResourcesManager) {
            final int NAPP = mAllApplications.size();
            for (int i=0; i<NAPP; i++) {
                callbacks.add(mAllApplications.get(i));
            }
            final int NACT = mActivities.size();
            for (int i=0; i<NACT; i++) {
                ActivityClientRecord ar = mActivities.valueAt(i);
                Activity a = ar.activity;
                if (a != null) {
                    Configuration thisConfig = applyConfigCompatMainThread(
                            mCurDefaultDisplayDpi, newConfig,
                            ar.packageInfo.getCompatibilityInfo());
                    if (!ar.activity.mFinished && (allActivities || !ar.paused)) {
                        // If the activity is currently resumed, its configuration
                        // needs to change right now.
                        callbacks.add(a);
                    } else if (thisConfig != null) {
                        // Otherwise, we will tell it about the change
                        // the next time it is resumed or shown.  Note that
                        // the activity manager may, before then, decide the
                        // activity needs to be destroyed to handle its new
                        // configuration.
                        if (DEBUG_CONFIGURATION) {
                            Slog.v(TAG, "Setting activity "
                                    + ar.activityInfo.name + " newConfig=" + thisConfig);
                        }
                        ar.newConfig = thisConfig;
                    }
                }
            }
            final int NSVC = mServices.size();
            for (int i=0; i<NSVC; i++) {
                callbacks.add(mServices.valueAt(i));
            }
        }
        synchronized (mProviderMap) {
            final int NPRV = mLocalProviders.size();
            for (int i=0; i<NPRV; i++) {
                callbacks.add(mLocalProviders.valueAt(i).mLocalProvider);
            }
        }

        return callbacks;
    }

    /**
     * Updates the configuration for an Activity. The ActivityClientRecord's
     * {@link ActivityClientRecord#overrideConfig} is used to compute the final Configuration for
     * that Activity. {@link ActivityClientRecord#tmpConfig} is used as a temporary for delivering
     * the updated Configuration.
     * @param r ActivityClientRecord representing the Activity.
     * @param newBaseConfig The new configuration to use. This may be augmented with
     *                      {@link ActivityClientRecord#overrideConfig}.
     * @param reportToActivity true if the change should be reported to the Activity's callback.
     */
    private void performConfigurationChangedForActivity(ActivityClientRecord r,
                                                        Configuration newBaseConfig,
                                                        boolean reportToActivity) {
        r.tmpConfig.setTo(newBaseConfig);
        if (r.overrideConfig != null) {
            r.tmpConfig.updateFrom(r.overrideConfig);
        }
        performConfigurationChanged(r.activity, r.token, r.tmpConfig, r.overrideConfig,
                reportToActivity);
        freeTextLayoutCachesIfNeeded(r.activity.mCurrentConfig.diff(r.tmpConfig));
    }

    /**
     * Creates a new Configuration only if override would modify base. Otherwise returns base.
     * @param base The base configuration.
     * @param override The update to apply to the base configuration. Can be null.
     * @return A Configuration representing base with override applied.
     */
    private static Configuration createNewConfigAndUpdateIfNotNull(@NonNull Configuration base,
            @Nullable Configuration override) {
        if (override == null) {
            return base;
        }
        Configuration newConfig = new Configuration(base);
        newConfig.updateFrom(override);
        return newConfig;
    }

    /**
     * Decides whether to update an Activity's configuration and whether to tell the
     * Activity/Component about it.
     * @param cb The component callback to notify of configuration change.
     * @param activityToken The Activity binder token for which this configuration change happened.
     *                      If the change is global, this is null.
     * @param newConfig The new configuration.
     * @param amOverrideConfig The override config that differentiates the Activity's configuration
     *                       from the base global configuration.
     *                       This is supplied by ActivityManager.
     * @param reportToActivity Notify the Activity of the change.
     */
    private void performConfigurationChanged(ComponentCallbacks2 cb,
                                             IBinder activityToken,
                                             Configuration newConfig,
                                             Configuration amOverrideConfig,
                                             boolean reportToActivity) {
        // Only for Activity objects, check that they actually call up to their
        // superclass implementation.  ComponentCallbacks2 is an interface, so
        // we check the runtime type and act accordingly.
        Activity activity = (cb instanceof Activity) ? (Activity) cb : null;
        if (activity != null) {
            activity.mCalled = false;
        }

        boolean shouldChangeConfig = false;
        if ((activity == null) || (activity.mCurrentConfig == null)) {
            shouldChangeConfig = true;
        } else {
            // If the new config is the same as the config this Activity is already
            // running with and the override config also didn't change, then don't
            // bother calling onConfigurationChanged.
            int diff = activity.mCurrentConfig.diff(newConfig);
            if (diff != 0 || !mResourcesManager.isSameResourcesOverrideConfig(activityToken,
                    amOverrideConfig)) {
                // Always send the task-level config changes. For system-level configuration, if
                // this activity doesn't handle any of the config changes, then don't bother
                // calling onConfigurationChanged as we're going to destroy it.
                if (!mUpdatingSystemConfig
                        || (~activity.mActivityInfo.getRealConfigChanged() & diff) == 0
                        || !reportToActivity) {
                    shouldChangeConfig = true;
                }
            }
        }

        if (shouldChangeConfig) {
            // Propagate the configuration change to the Activity and ResourcesManager.

            // ContextThemeWrappers may override the configuration for that context.
            // We must check and apply any overrides defined.
            Configuration contextThemeWrapperOverrideConfig = null;
            if (cb instanceof ContextThemeWrapper) {
                final ContextThemeWrapper contextThemeWrapper = (ContextThemeWrapper) cb;
                contextThemeWrapperOverrideConfig = contextThemeWrapper.getOverrideConfiguration();
            }

            // We only update an Activity's configuration if this is not a global
            // configuration change. This must also be done before the callback,
            // or else we violate the contract that the new resources are available
            // in {@link ComponentCallbacks2#onConfigurationChanged(Configuration)}.
            if (activityToken != null) {
                // Apply the ContextThemeWrapper override if necessary.
                // NOTE: Make sure the configurations are not modified, as they are treated
                // as immutable in many places.
                final Configuration finalOverrideConfig = createNewConfigAndUpdateIfNotNull(
                        amOverrideConfig, contextThemeWrapperOverrideConfig);
                mResourcesManager.updateResourcesForActivity(activityToken, finalOverrideConfig);
            }

            if (reportToActivity) {
                // Apply the ContextThemeWrapper override if necessary.
                // NOTE: Make sure the configurations are not modified, as they are treated
                // as immutable in many places.
                final Configuration configToReport = createNewConfigAndUpdateIfNotNull(
                        newConfig, contextThemeWrapperOverrideConfig);
                cb.onConfigurationChanged(configToReport);
            }

            if (activity != null) {
                if (reportToActivity && !activity.mCalled) {
                    throw new SuperNotCalledException(
                            "Activity " + activity.getLocalClassName() +
                            " did not call through to super.onConfigurationChanged()");
                }
                activity.mConfigChangeFlags = 0;
                activity.mCurrentConfig = new Configuration(newConfig);
            }
        }
    }

    public final void applyConfigurationToResources(Configuration config) {
        synchronized (mResourcesManager) {
            mResourcesManager.applyConfigurationToResourcesLocked(config, null);
        }
    }

    final Configuration applyCompatConfiguration(int displayDensity) {
        Configuration config = mConfiguration;
        if (mCompatConfiguration == null) {
            mCompatConfiguration = new Configuration();
        }
        mCompatConfiguration.setTo(mConfiguration);
        if (mResourcesManager.applyCompatConfigurationLocked(displayDensity,
                mCompatConfiguration)) {
            config = mCompatConfiguration;
        }
        return config;
    }

    final void handleConfigurationChanged(Configuration config, CompatibilityInfo compat) {

        int configDiff = 0;

        synchronized (mResourcesManager) {
            if (mPendingConfiguration != null) {
                if (!mPendingConfiguration.isOtherSeqNewer(config)) {
                    config = mPendingConfiguration;
                    mCurDefaultDisplayDpi = config.densityDpi;
                    updateDefaultDensity();
                }
                mPendingConfiguration = null;
            }

            if (config == null) {
                return;
            }

            if (DEBUG_CONFIGURATION) Slog.v(TAG, "Handle configuration changed: "
                    + config);

            mResourcesManager.applyConfigurationToResourcesLocked(config, compat);
            updateLocaleListFromAppContext(mInitialApplication.getApplicationContext(),
                    mResourcesManager.getConfiguration().getLocales());

            if (mConfiguration == null) {
                mConfiguration = new Configuration();
            }
            if (!mConfiguration.isOtherSeqNewer(config) && compat == null) {
                return;
            }

            configDiff = mConfiguration.updateFrom(config);
            config = applyCompatConfiguration(mCurDefaultDisplayDpi);

            final Theme systemTheme = getSystemContext().getTheme();
            if ((systemTheme.getChangingConfigurations() & configDiff) != 0) {
                systemTheme.rebase();
            }
        }

        ArrayList<ComponentCallbacks2> callbacks = collectComponentCallbacks(false, config);

        freeTextLayoutCachesIfNeeded(configDiff);

        if (callbacks != null) {
            final int N = callbacks.size();
            for (int i=0; i<N; i++) {
                ComponentCallbacks2 cb = callbacks.get(i);
                if (cb instanceof Activity) {
                    // If callback is an Activity - call corresponding method to consider override
                    // config and avoid onConfigurationChanged if it hasn't changed.
                    Activity a = (Activity) cb;
                    performConfigurationChangedForActivity(mActivities.get(a.getActivityToken()),
                            config, REPORT_TO_ACTIVITY);
                } else {
                    performConfigurationChanged(cb, null, config, null, REPORT_TO_ACTIVITY);
                }
            }
        }
    }

    static void freeTextLayoutCachesIfNeeded(int configDiff) {
        if (configDiff != 0) {
            // Ask text layout engine to free its caches if there is a locale change
            boolean hasLocaleConfigChange = ((configDiff & ActivityInfo.CONFIG_LOCALE) != 0);
            if (hasLocaleConfigChange) {
                Canvas.freeTextLayoutCaches();
                if (DEBUG_CONFIGURATION) Slog.v(TAG, "Cleared TextLayout Caches");
            }
        }
    }

    final void handleActivityConfigurationChanged(ActivityConfigChangeData data,
            boolean reportToActivity) {
        ActivityClientRecord r = mActivities.get(data.activityToken);
        if (r == null || r.activity == null) {
            return;
        }

        if (DEBUG_CONFIGURATION) Slog.v(TAG, "Handle activity config changed: "
                + r.activityInfo.name + ", with callback=" + reportToActivity);

        r.overrideConfig = data.overrideConfig;
        performConfigurationChangedForActivity(r, mCompatConfiguration, reportToActivity);
        mSomeActivitiesChanged = true;
    }

    final void handleProfilerControl(boolean start, ProfilerInfo profilerInfo, int profileType) {
        if (start) {
            try {
                switch (profileType) {
                    default:
                        mProfiler.setProfiler(profilerInfo);
                        mProfiler.startProfiling();
                        break;
                }
            } catch (RuntimeException e) {
                Slog.w(TAG, "Profiling failed on path " + profilerInfo.profileFile
                        + " -- can the process access this path?");
            } finally {
                try {
                    profilerInfo.profileFd.close();
                } catch (IOException e) {
                    Slog.w(TAG, "Failure closing profile fd", e);
                }
            }
        } else {
            switch (profileType) {
                default:
                    mProfiler.stopProfiling();
                    break;
            }
        }
    }

    /**
     * Public entrypoint to stop profiling. This is required to end profiling when the app crashes,
     * so that profiler data won't be lost.
     *
     * @hide
     */
    public void stopProfiling() {
        mProfiler.stopProfiling();
    }

    static final void handleDumpHeap(boolean managed, DumpHeapData dhd) {
        if (managed) {
            try {
                Debug.dumpHprofData(dhd.path, dhd.fd.getFileDescriptor());
            } catch (IOException e) {
                Slog.w(TAG, "Managed heap dump failed on path " + dhd.path
                        + " -- can the process access this path?");
            } finally {
                try {
                    dhd.fd.close();
                } catch (IOException e) {
                    Slog.w(TAG, "Failure closing profile fd", e);
                }
            }
        } else {
            Debug.dumpNativeHeap(dhd.fd.getFileDescriptor());
        }
        try {
            ActivityManagerNative.getDefault().dumpHeapFinished(dhd.path);
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        }
    }

    final void handleDispatchPackageBroadcast(int cmd, String[] packages) {
        boolean hasPkgInfo = false;
        switch (cmd) {
            case IApplicationThread.PACKAGE_REMOVED:
            case IApplicationThread.PACKAGE_REMOVED_DONT_KILL:
            {
                final boolean killApp = cmd == IApplicationThread.PACKAGE_REMOVED;
                if (packages == null) {
                    break;
                }
                synchronized (mResourcesManager) {
                    for (int i = packages.length - 1; i >= 0; i--) {
                        if (!hasPkgInfo) {
                            WeakReference<LoadedApk> ref = mPackages.get(packages[i]);
                            if (ref != null && ref.get() != null) {
                                hasPkgInfo = true;
                            } else {
                                ref = mResourcePackages.get(packages[i]);
                                if (ref != null && ref.get() != null) {
                                    hasPkgInfo = true;
                                }
                            }
                        }
                        if (killApp) {
                            mPackages.remove(packages[i]);
                            mResourcePackages.remove(packages[i]);
                        }
                    }
                }
                break;
            }
            case IApplicationThread.PACKAGE_REPLACED:
            {
                if (packages == null) {
                    break;
                }
                synchronized (mResourcesManager) {
                    for (int i = packages.length - 1; i >= 0; i--) {
                        WeakReference<LoadedApk> ref = mPackages.get(packages[i]);
                        LoadedApk pkgInfo = ref != null ? ref.get() : null;
                        if (pkgInfo != null) {
                            hasPkgInfo = true;
                        } else {
                            ref = mResourcePackages.get(packages[i]);
                            pkgInfo = ref != null ? ref.get() : null;
                            if (pkgInfo != null) {
                                hasPkgInfo = true;
                            }
                        }
                        // If the package is being replaced, yet it still has a valid
                        // LoadedApk object, the package was updated with _DONT_KILL.
                        // Adjust it's internal references to the application info and
                        // resources.
                        if (pkgInfo != null) {
                            try {
                                final String packageName = packages[i];
                                final ApplicationInfo aInfo =
                                        sPackageManager.getApplicationInfo(
                                                packageName,
                                                0 /*flags*/,
                                                UserHandle.myUserId());

                                if (mActivities.size() > 0) {
                                    for (ActivityClientRecord ar : mActivities.values()) {
                                        if (ar.activityInfo.applicationInfo.packageName
                                                .equals(packageName)) {
                                            ar.activityInfo.applicationInfo = aInfo;
                                            ar.packageInfo = pkgInfo;
                                        }
                                    }
                                }
                                final List<String> oldPaths =
                                        sPackageManager.getPreviousCodePaths(packageName);
                                pkgInfo.updateApplicationInfo(aInfo, oldPaths);
                            } catch (RemoteException e) {
                            }
                        }
                    }
                }
                break;
            }
        }
        ApplicationPackageManager.handlePackageBroadcast(cmd, packages, hasPkgInfo);
    }

    final void handleLowMemory() {
        ArrayList<ComponentCallbacks2> callbacks = collectComponentCallbacks(true, null);

        final int N = callbacks.size();
        for (int i=0; i<N; i++) {
            callbacks.get(i).onLowMemory();
        }

        // Ask SQLite to free up as much memory as it can, mostly from its page caches.
        if (Process.myUid() != Process.SYSTEM_UID) {
            int sqliteReleased = SQLiteDatabase.releaseMemory();
            EventLog.writeEvent(SQLITE_MEM_RELEASED_EVENT_LOG_TAG, sqliteReleased);
        }

        // Ask graphics to free up as much as possible (font/image caches)
        Canvas.freeCaches();

        // Ask text layout engine to free also as much as possible
        Canvas.freeTextLayoutCaches();

        BinderInternal.forceGc("mem");
    }

    final void handleTrimMemory(int level) {
        if (DEBUG_MEMORY_TRIM) Slog.v(TAG, "Trimming memory to level: " + level);

        ArrayList<ComponentCallbacks2> callbacks = collectComponentCallbacks(true, null);

        final int N = callbacks.size();
        for (int i = 0; i < N; i++) {
            callbacks.get(i).onTrimMemory(level);
        }

        WindowManagerGlobal.getInstance().trimMemory(level);
    }

    private void setupGraphicsSupport(Context context, File cacheDir) {
        if (Process.isIsolated()) {
            // Isolated processes aren't going to do UI.
            return;
        }
        Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "setupGraphicsSupport");
        try {
            int uid = Process.myUid();
            String[] packages = getPackageManager().getPackagesForUid(uid);

            if (packages != null) {
                ThreadedRenderer.setupDiskCache(cacheDir);
                RenderScriptCacheDir.setupDiskCache(cacheDir);
                GraphicsEnvironment.setupGraphicsEnvironment(context);
            }
        } catch (RemoteException e) {
            throw e.rethrowFromSystemServer();
        } finally {
            Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
        }
    }

    private void updateDefaultDensity() {
        final int densityDpi = mCurDefaultDisplayDpi;
        if (!mDensityCompatMode
                && densityDpi != Configuration.DENSITY_DPI_UNDEFINED
                && densityDpi != DisplayMetrics.DENSITY_DEVICE) {
            DisplayMetrics.DENSITY_DEVICE = densityDpi;
            Bitmap.setDefaultDensity(densityDpi);
        }
    }

    /**
     * Returns the correct library directory for the current ABI.
     * <p>
     * If we're dealing with a multi-arch application that has both 32 and 64 bit shared
     * libraries, we might need to choose the secondary depending on what the current
     * runtime's instruction set is.
     */
    private String getInstrumentationLibrary(ApplicationInfo appInfo, InstrumentationInfo insInfo) {
        if (appInfo.primaryCpuAbi != null && appInfo.secondaryCpuAbi != null) {
            // Get the instruction set supported by the secondary ABI. In the presence
            // of a native bridge this might be different than the one secondary ABI used.
            String secondaryIsa =
                    VMRuntime.getInstructionSet(appInfo.secondaryCpuAbi);
            final String secondaryDexCodeIsa =
                    SystemProperties.get("ro.dalvik.vm.isa." + secondaryIsa);
            secondaryIsa = secondaryDexCodeIsa.isEmpty() ? secondaryIsa : secondaryDexCodeIsa;

            final String runtimeIsa = VMRuntime.getRuntime().vmInstructionSet();
            if (runtimeIsa.equals(secondaryIsa)) {
                return insInfo.secondaryNativeLibraryDir;
            }
        }
        return insInfo.nativeLibraryDir;
    }

    /**
     * The LocaleList set for the app's resources may have been shuffled so that the preferred
     * Locale is at position 0. We must find the index of this preferred Locale in the
     * original LocaleList.
     */
    private void updateLocaleListFromAppContext(Context context, LocaleList newLocaleList) {
        final Locale bestLocale = context.getResources().getConfiguration().getLocales().get(0);
        final int newLocaleListSize = newLocaleList.size();
        for (int i = 0; i < newLocaleListSize; i++) {
            if (bestLocale.equals(newLocaleList.get(i))) {
                LocaleList.setDefault(newLocaleList, i);
                return;
            }
        }

        // The app may have overridden the LocaleList with its own Locale
        // (not present in the available list). Push the chosen Locale
        // to the front of the list.
        LocaleList.setDefault(new LocaleList(bestLocale, newLocaleList));
    }

    private void handleBindApplication(AppBindData data) {
        // Register the UI Thread as a sensitive thread to the runtime.
        VMRuntime.registerSensitiveThread();
        if (data.trackAllocation) {
            DdmVmInternal.enableRecentAllocations(true);
        }

        // Note when this process has started.
        Process.setStartTimes(SystemClock.elapsedRealtime(), SystemClock.uptimeMillis());

        mBoundApplication = data;
        mConfiguration = new Configuration(data.config);
        mCompatConfiguration = new Configuration(data.config);

        mProfiler = new Profiler();
        if (data.initProfilerInfo != null) {
            mProfiler.profileFile = data.initProfilerInfo.profileFile;
            mProfiler.profileFd = data.initProfilerInfo.profileFd;
            mProfiler.samplingInterval = data.initProfilerInfo.samplingInterval;
            mProfiler.autoStopProfiler = data.initProfilerInfo.autoStopProfiler;
        }

        // send up app name; do this *before* waiting for debugger
        Process.setArgV0(data.processName);
        android.ddm.DdmHandleAppName.setAppName(data.processName,
                                                UserHandle.myUserId());

        if (data.persistent) {
            // Persistent processes on low-memory devices do not get to
            // use hardware accelerated drawing, since this can add too much
            // overhead to the process.
            if (!ActivityManager.isHighEndGfx()) {
                ThreadedRenderer.disable(false);
            }
        }

        if (mProfiler.profileFd != null) {
            mProfiler.startProfiling();
        }

        // If the app is Honeycomb MR1 or earlier, switch its AsyncTask
        // implementation to use the pool executor.  Normally, we use the
        // serialized executor as the default. This has to happen in the
        // main thread so the main looper is set right.
        if (data.appInfo.targetSdkVersion <= android.os.Build.VERSION_CODES.HONEYCOMB_MR1) {
            AsyncTask.setDefaultExecutor(AsyncTask.THREAD_POOL_EXECUTOR);
        }

        Message.updateCheckRecycle(data.appInfo.targetSdkVersion);

        /*
         * Before spawning a new process, reset the time zone to be the system time zone.
         * This needs to be done because the system time zone could have changed after the
         * the spawning of this process. Without doing this this process would have the incorrect
         * system time zone.
         */
        TimeZone.setDefault(null);

        /*
         * Set the LocaleList. This may change once we create the App Context.
         */
        LocaleList.setDefault(data.config.getLocales());

        synchronized (mResourcesManager) {
            /*
             * Update the system configuration since its preloaded and might not
             * reflect configuration changes. The configuration object passed
             * in AppBindData can be safely assumed to be up to date
             */
            mResourcesManager.applyConfigurationToResourcesLocked(data.config, data.compatInfo);
            mCurDefaultDisplayDpi = data.config.densityDpi;

            // This calls mResourcesManager so keep it within the synchronized block.
            applyCompatConfiguration(mCurDefaultDisplayDpi);
        }

        data.info = getPackageInfoNoCheck(data.appInfo, data.compatInfo);

        /**
         * Switch this process to density compatibility mode if needed.
         */
        if ((data.appInfo.flags&ApplicationInfo.FLAG_SUPPORTS_SCREEN_DENSITIES)
                == 0) {
            mDensityCompatMode = true;
            Bitmap.setDefaultDensity(DisplayMetrics.DENSITY_DEFAULT);
        }
        updateDefaultDensity();

        final String use24HourSetting = mCoreSettings.getString(Settings.System.TIME_12_24);
        Boolean is24Hr = null;
        if (use24HourSetting != null) {
            is24Hr = "24".equals(use24HourSetting) ? Boolean.TRUE : Boolean.FALSE;
        }
        // null : use locale default for 12/24 hour formatting,
        // false : use 12 hour format,
        // true : use 24 hour format.
        DateFormat.set24HourTimePref(is24Hr);

        View.mDebugViewAttributes =
                mCoreSettings.getInt(Settings.Global.DEBUG_VIEW_ATTRIBUTES, 0) != 0;

        /**
         * For system applications on userdebug/eng builds, log stack
         * traces of disk and network access to dropbox for analysis.
         */
        if ((data.appInfo.flags &
             (ApplicationInfo.FLAG_SYSTEM |
              ApplicationInfo.FLAG_UPDATED_SYSTEM_APP)) != 0) {
            StrictMode.conditionallyEnableDebugLogging();
        }

        /**
         * For apps targetting Honeycomb or later, we don't allow network usage
         * on the main event loop / UI thread. This is what ultimately throws
         * {@link NetworkOnMainThreadException}.
         */
        if (data.appInfo.targetSdkVersion >= Build.VERSION_CODES.HONEYCOMB) {
            StrictMode.enableDeathOnNetwork();
        }

        /**
         * For apps targetting N or later, we don't allow file:// Uri exposure.
         * This is what ultimately throws {@link FileUriExposedException}.
         */
        if (data.appInfo.targetSdkVersion >= Build.VERSION_CODES.N) {
            StrictMode.enableDeathOnFileUriExposure();
        }

        NetworkSecurityPolicy.getInstance().setCleartextTrafficPermitted(
                (data.appInfo.flags & ApplicationInfo.FLAG_USES_CLEARTEXT_TRAFFIC) != 0);

        if (data.debugMode != IApplicationThread.DEBUG_OFF) {
            // XXX should have option to change the port.
            Debug.changeDebugPort(8100);
            if (data.debugMode == IApplicationThread.DEBUG_WAIT) {
                Slog.w(TAG, "Application " + data.info.getPackageName()
                      + " is waiting for the debugger on port 8100...");

                IActivityManager mgr = ActivityManagerNative.getDefault();
                try {
                    mgr.showWaitingForDebugger(mAppThread, true);
                } catch (RemoteException ex) {
                    throw ex.rethrowFromSystemServer();
                }

                Debug.waitForDebugger();

                try {
                    mgr.showWaitingForDebugger(mAppThread, false);
                } catch (RemoteException ex) {
                    throw ex.rethrowFromSystemServer();
                }

            } else {
                Slog.w(TAG, "Application " + data.info.getPackageName()
                      + " can be debugged on port 8100...");
            }
        }

        // Allow application-generated systrace messages if we're debuggable.
        boolean isAppDebuggable = (data.appInfo.flags & ApplicationInfo.FLAG_DEBUGGABLE) != 0;
        Trace.setAppTracingAllowed(isAppDebuggable);
        if (isAppDebuggable && data.enableBinderTracking) {
            Binder.enableTracing();
        }

        /**
         * Initialize the default http proxy in this process for the reasons we set the time zone.
         */
        Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "Setup proxies");
        final IBinder b = ServiceManager.getService(Context.CONNECTIVITY_SERVICE);
        if (b != null) {
            // In pre-boot mode (doing initial launch to collect password), not
            // all system is up.  This includes the connectivity service, so don't
            // crash if we can't get it.
            final IConnectivityManager service = IConnectivityManager.Stub.asInterface(b);
            try {
                final ProxyInfo proxyInfo = service.getProxyForNetwork(null);
                Proxy.setHttpProxySystemProperty(proxyInfo);
            } catch (RemoteException e) {
                Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
                throw e.rethrowFromSystemServer();
            }
        }
        Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);

        // Instrumentation info affects the class loader, so load it before
        // setting up the app context.
        final InstrumentationInfo ii;
        if (data.instrumentationName != null) {
            try {
                ii = new ApplicationPackageManager(null, getPackageManager())
                        .getInstrumentationInfo(data.instrumentationName, 0);
            } catch (PackageManager.NameNotFoundException e) {
                throw new RuntimeException(
                        "Unable to find instrumentation info for: " + data.instrumentationName);
            }

            mInstrumentationPackageName = ii.packageName;
            mInstrumentationAppDir = ii.sourceDir;
            mInstrumentationSplitAppDirs = ii.splitSourceDirs;
            mInstrumentationLibDir = getInstrumentationLibrary(data.appInfo, ii);
            mInstrumentedAppDir = data.info.getAppDir();
            mInstrumentedSplitAppDirs = data.info.getSplitAppDirs();
            mInstrumentedLibDir = data.info.getLibDir();
        } else {
            ii = null;
        }

        final ContextImpl appContext = ContextImpl.createAppContext(this, data.info);
        updateLocaleListFromAppContext(appContext,
                mResourcesManager.getConfiguration().getLocales());

        if (!Process.isIsolated() && !"android".equals(appContext.getPackageName())) {
            // This cache location probably points at credential-encrypted
            // storage which may not be accessible yet; assign it anyway instead
            // of pointing at device-encrypted storage.
            final File cacheDir = appContext.getCacheDir();
            if (cacheDir != null) {
                // Provide a usable directory for temporary files
                System.setProperty("java.io.tmpdir", cacheDir.getAbsolutePath());
            } else {
                Log.v(TAG, "Unable to initialize \"java.io.tmpdir\" property "
                        + "due to missing cache directory");
            }

            // Setup a location to store generated/compiled graphics code.
            final Context deviceContext = appContext.createDeviceProtectedStorageContext();
            final File codeCacheDir = deviceContext.getCodeCacheDir();
            if (codeCacheDir != null) {
                setupGraphicsSupport(appContext, codeCacheDir);
            } else {
                Log.e(TAG, "Unable to setupGraphicsSupport due to missing code-cache directory");
            }
        }

        // Install the Network Security Config Provider. This must happen before the application
        // code is loaded to prevent issues with instances of TLS objects being created before
        // the provider is installed.
        Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "NetworkSecurityConfigProvider.install");
        NetworkSecurityConfigProvider.install(appContext);
        Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);

        // Continue loading instrumentation.
        if (ii != null) {
            final ApplicationInfo instrApp = new ApplicationInfo();
            ii.copyTo(instrApp);
            instrApp.initForUser(UserHandle.myUserId());
            final LoadedApk pi = getPackageInfo(instrApp, data.compatInfo,
                    appContext.getClassLoader(), false, true, false);
            final ContextImpl instrContext = ContextImpl.createAppContext(this, pi);

            try {
                final ClassLoader cl = instrContext.getClassLoader();
                mInstrumentation = (Instrumentation)
                    cl.loadClass(data.instrumentationName.getClassName()).newInstance();
            } catch (Exception e) {
                throw new RuntimeException(
                    "Unable to instantiate instrumentation "
                    + data.instrumentationName + ": " + e.toString(), e);
            }

            final ComponentName component = new ComponentName(ii.packageName, ii.name);
            mInstrumentation.init(this, instrContext, appContext, component,
                    data.instrumentationWatcher, data.instrumentationUiAutomationConnection);

            if (mProfiler.profileFile != null && !ii.handleProfiling
                    && mProfiler.profileFd == null) {
                mProfiler.handlingProfiling = true;
                final File file = new File(mProfiler.profileFile);
                file.getParentFile().mkdirs();
                Debug.startMethodTracing(file.toString(), 8 * 1024 * 1024);
            }
        } else {
            mInstrumentation = new Instrumentation();
        }

        if ((data.appInfo.flags&ApplicationInfo.FLAG_LARGE_HEAP) != 0) {
            dalvik.system.VMRuntime.getRuntime().clearGrowthLimit();
        } else {
            // Small heap, clamp to the current growth limit and let the heap release
            // pages after the growth limit to the non growth limit capacity. b/18387825
            dalvik.system.VMRuntime.getRuntime().clampGrowthLimit();
        }

        // Allow disk access during application and provider setup. This could
        // block processing ordered broadcasts, but later processing would
        // probably end up doing the same disk access.
        final StrictMode.ThreadPolicy savedPolicy = StrictMode.allowThreadDiskWrites();
        try {
            // If the app is being launched for full backup or restore, bring it up in
            // a restricted environment with the base application class.
            Application app = data.info.makeApplication(data.restrictedBackupMode, null);
            mInitialApplication = app;

            //patch by Youlor
            //++++++++++++++++++++++++++++
            Unpacker.unpack();
            //++++++++++++++++++++++++++++

            // don't bring up providers in restricted mode; they may depend on the
            // app's custom Application class
            if (!data.restrictedBackupMode) {
                if (!ArrayUtils.isEmpty(data.providers)) {
                    installContentProviders(app, data.providers);
                    // For process that contains content providers, we want to
                    // ensure that the JIT is enabled "at some point".
                    mH.sendEmptyMessageDelayed(H.ENABLE_JIT, 10*1000);
                }
            }

            // Do this after providers, since instrumentation tests generally start their
            // test thread at this point, and we don't want that racing.
            try {
                mInstrumentation.onCreate(data.instrumentationArgs);
            }
            catch (Exception e) {
                throw new RuntimeException(
                    "Exception thrown in onCreate() of "
                    + data.instrumentationName + ": " + e.toString(), e);
            }

            try {
                mInstrumentation.callApplicationOnCreate(app);
            } catch (Exception e) {
                if (!mInstrumentation.onException(app, e)) {
                    throw new RuntimeException(
                        "Unable to create application " + app.getClass().getName()
                        + ": " + e.toString(), e);
                }
            }
        } finally {
            StrictMode.setThreadPolicy(savedPolicy);
        }
    }

    /*package*/ final void finishInstrumentation(int resultCode, Bundle results) {
        IActivityManager am = ActivityManagerNative.getDefault();
        if (mProfiler.profileFile != null && mProfiler.handlingProfiling
                && mProfiler.profileFd == null) {
            Debug.stopMethodTracing();
        }
        //Slog.i(TAG, "am: " + ActivityManagerNative.getDefault()
        //      + ", app thr: " + mAppThread);
        try {
            am.finishInstrumentation(mAppThread, resultCode, results);
        } catch (RemoteException ex) {
            throw ex.rethrowFromSystemServer();
        }
    }

    private void installContentProviders(
            Context context, List<ProviderInfo> providers) {
        final ArrayList<IActivityManager.ContentProviderHolder> results =
            new ArrayList<IActivityManager.ContentProviderHolder>();

        for (ProviderInfo cpi : providers) {
            if (DEBUG_PROVIDER) {
                StringBuilder buf = new StringBuilder(128);
                buf.append("Pub ");
                buf.append(cpi.authority);
                buf.append(": ");
                buf.append(cpi.name);
                Log.i(TAG, buf.toString());
            }
            IActivityManager.ContentProviderHolder cph = installProvider(context, null, cpi,
                    false /*noisy*/, true /*noReleaseNeeded*/, true /*stable*/);
            if (cph != null) {
                cph.noReleaseNeeded = true;
                results.add(cph);
            }
        }

        try {
            ActivityManagerNative.getDefault().publishContentProviders(
                getApplicationThread(), results);
        } catch (RemoteException ex) {
            throw ex.rethrowFromSystemServer();
        }
    }

    public final IContentProvider acquireProvider(
            Context c, String auth, int userId, boolean stable) {
        final ProviderKey key = new ProviderKey(auth, userId);
        final IContentProvider provider = acquireExistingProvider(c, key, stable);
        if (provider != null) {
            return provider;
        }

        ProviderAcquiringCount pac;
        synchronized (mProviderMap) {
            pac = mProviderAcquiringCountMap.get(key);
            if (pac == null) {
                pac = new ProviderAcquiringCount(1);
                mProviderAcquiringCountMap.put(key, pac);
            } else {
                pac.acquiringCount++;
            }
        }

        // There is a possible race here.  Another thread may try to acquire
        // the same provider at the same time.  When this happens, we want to ensure
        // that the first one wins.
        // Note that we cannot hold the lock while acquiring and installing the
        // provider since it might take a long time to run and it could also potentially
        // be re-entrant in the case where the provider is in the same process.
        IActivityManager.ContentProviderHolder holder = null;
        synchronized (pac) {
            try {
                holder = ActivityManagerNative.getDefault().getContentProvider(
                        getApplicationThread(), auth, userId, stable);
            } catch (RemoteException ex) {
            }
        }
        synchronized (mProviderMap) {
            if(--pac.acquiringCount == 0) {
                mProviderAcquiringCountMap.remove(key);
            }
        }
        if (holder == null) {
            Slog.e(TAG, "Failed to find provider info for " + auth);
            return null;
        }

        // Install provider will increment the reference count for us, and break
        // any ties in the race.
        holder = installProvider(c, holder, holder.info,
                true /*noisy*/, holder.noReleaseNeeded, stable);
        return holder.provider;
    }

    private final void incProviderRefLocked(ProviderRefCount prc, boolean stable) {
        if (stable) {
            prc.stableCount += 1;
            if (prc.stableCount == 1) {
                // We are acquiring a new stable reference on the provider.
                int unstableDelta;
                if (prc.removePending) {
                    // We have a pending remove operation, which is holding the
                    // last unstable reference.  At this point we are converting
                    // that unstable reference to our new stable reference.
                    unstableDelta = -1;
                    // Cancel the removal of the provider.
                    if (DEBUG_PROVIDER) {
                        Slog.v(TAG, "incProviderRef: stable "
                                + "snatched provider from the jaws of death");
                    }
                    prc.removePending = false;
                    // There is a race! It fails to remove the message, which
                    // will be handled in completeRemoveProvider().
                    mH.removeMessages(H.REMOVE_PROVIDER, prc);
                } else {
                    unstableDelta = 0;
                }
                try {
                    if (DEBUG_PROVIDER) {
                        Slog.v(TAG, "incProviderRef Now stable - "
                                + prc.holder.info.name + ": unstableDelta="
                                + unstableDelta);
                    }
                    ActivityManagerNative.getDefault().refContentProvider(
                            prc.holder.connection, 1, unstableDelta);
                } catch (RemoteException e) {
                    //do nothing content provider object is dead any way
                }
            }
        } else {
            prc.unstableCount += 1;
            if (prc.unstableCount == 1) {
                // We are acquiring a new unstable reference on the provider.
                if (prc.removePending) {
                    // Oh look, we actually have a remove pending for the
                    // provider, which is still holding the last unstable
                    // reference.  We just need to cancel that to take new
                    // ownership of the reference.
                    if (DEBUG_PROVIDER) {
                        Slog.v(TAG, "incProviderRef: unstable "
                                + "snatched provider from the jaws of death");
                    }
                    prc.removePending = false;
                    mH.removeMessages(H.REMOVE_PROVIDER, prc);
                } else {
                    // First unstable ref, increment our count in the
                    // activity manager.
                    try {
                        if (DEBUG_PROVIDER) {
                            Slog.v(TAG, "incProviderRef: Now unstable - "
                                    + prc.holder.info.name);
                        }
                        ActivityManagerNative.getDefault().refContentProvider(
                                prc.holder.connection, 0, 1);
                    } catch (RemoteException e) {
                        //do nothing content provider object is dead any way
                    }
                }
            }
        }
    }

    public final IContentProvider acquireExistingProvider(
            Context c, String auth, int userId, boolean stable) {
        return acquireExistingProvider(c, new ProviderKey(auth, userId), stable);
    }

    public final IContentProvider acquireExistingProvider(
            Context c, ProviderKey key, boolean stable) {
        synchronized (mProviderMap) {
            final ProviderClientRecord pr = mProviderMap.get(key);
            if (pr == null) {
                return null;
            }

            IContentProvider provider = pr.mProvider;
            IBinder jBinder = provider.asBinder();
            if (!jBinder.isBinderAlive()) {
                // The hosting process of the provider has died; we can't
                // use this one.
                Log.i(TAG, "Acquiring provider " + key.authority + " for user " + key.userId
                        + ": existing object's process dead");
                handleUnstableProviderDiedLocked(jBinder, true);
                return null;
            }

            // Only increment the ref count if we have one.  If we don't then the
            // provider is not reference counted and never needs to be released.
            ProviderRefCount prc = mProviderRefCountMap.get(jBinder);
            if (prc != null) {
                incProviderRefLocked(prc, stable);
            }
            return provider;
        }
    }

    public final boolean releaseProvider(IContentProvider provider, boolean stable) {
        if (provider == null) {
            return false;
        }

        IBinder jBinder = provider.asBinder();
        synchronized (mProviderMap) {
            ProviderRefCount prc = mProviderRefCountMap.get(jBinder);
            if (prc == null) {
                // The provider has no ref count, no release is needed.
                return false;
            }

            boolean lastRef = false;
            if (stable) {
                if (prc.stableCount == 0) {
                    if (DEBUG_PROVIDER) Slog.v(TAG,
                            "releaseProvider: stable ref count already 0, how?");
                    return false;
                }
                prc.stableCount -= 1;
                if (prc.stableCount == 0) {
                    // What we do at this point depends on whether there are
                    // any unstable refs left: if there are, we just tell the
                    // activity manager to decrement its stable count; if there
                    // aren't, we need to enqueue this provider to be removed,
                    // and convert to holding a single unstable ref while
                    // doing so.
                    lastRef = prc.unstableCount == 0;
                    try {
                        if (DEBUG_PROVIDER) {
                            Slog.v(TAG, "releaseProvider: No longer stable w/lastRef="
                                    + lastRef + " - " + prc.holder.info.name);
                        }
                        ActivityManagerNative.getDefault().refContentProvider(
                                prc.holder.connection, -1, lastRef ? 1 : 0);
                    } catch (RemoteException e) {
                        //do nothing content provider object is dead any way
                    }
                }
            } else {
                if (prc.unstableCount == 0) {
                    if (DEBUG_PROVIDER) Slog.v(TAG,
                            "releaseProvider: unstable ref count already 0, how?");
                    return false;
                }
                prc.unstableCount -= 1;
                if (prc.unstableCount == 0) {
                    // If this is the last reference, we need to enqueue
                    // this provider to be removed instead of telling the
                    // activity manager to remove it at this point.
                    lastRef = prc.stableCount == 0;
                    if (!lastRef) {
                        try {
                            if (DEBUG_PROVIDER) {
                                Slog.v(TAG, "releaseProvider: No longer unstable - "
                                        + prc.holder.info.name);
                            }
                            ActivityManagerNative.getDefault().refContentProvider(
                                    prc.holder.connection, 0, -1);
                        } catch (RemoteException e) {
                            //do nothing content provider object is dead any way
                        }
                    }
                }
            }

            if (lastRef) {
                if (!prc.removePending) {
                    // Schedule the actual remove asynchronously, since we don't know the context
                    // this will be called in.
                    // TODO: it would be nice to post a delayed message, so
                    // if we come back and need the same provider quickly
                    // we will still have it available.
                    if (DEBUG_PROVIDER) {
                        Slog.v(TAG, "releaseProvider: Enqueueing pending removal - "
                                + prc.holder.info.name);
                    }
                    prc.removePending = true;
                    Message msg = mH.obtainMessage(H.REMOVE_PROVIDER, prc);
                    mH.sendMessage(msg);
                } else {
                    Slog.w(TAG, "Duplicate remove pending of provider " + prc.holder.info.name);
                }
            }
            return true;
        }
    }

    final void completeRemoveProvider(ProviderRefCount prc) {
        synchronized (mProviderMap) {
            if (!prc.removePending) {
                // There was a race!  Some other client managed to acquire
                // the provider before the removal was completed.
                // Abort the removal.  We will do it later.
                if (DEBUG_PROVIDER) Slog.v(TAG, "completeRemoveProvider: lost the race, "
                        + "provider still in use");
                return;
            }

            // More complicated race!! Some client managed to acquire the
            // provider and release it before the removal was completed.
            // Continue the removal, and abort the next remove message.
            prc.removePending = false;

            final IBinder jBinder = prc.holder.provider.asBinder();
            ProviderRefCount existingPrc = mProviderRefCountMap.get(jBinder);
            if (existingPrc == prc) {
                mProviderRefCountMap.remove(jBinder);
            }

            for (int i=mProviderMap.size()-1; i>=0; i--) {
                ProviderClientRecord pr = mProviderMap.valueAt(i);
                IBinder myBinder = pr.mProvider.asBinder();
                if (myBinder == jBinder) {
                    mProviderMap.removeAt(i);
                }
            }
        }

        try {
            if (DEBUG_PROVIDER) {
                Slog.v(TAG, "removeProvider: Invoking ActivityManagerNative."
                        + "removeContentProvider(" + prc.holder.info.name + ")");
            }
            ActivityManagerNative.getDefault().removeContentProvider(
                    prc.holder.connection, false);
        } catch (RemoteException e) {
            //do nothing content provider object is dead any way
        }
    }

    final void handleUnstableProviderDied(IBinder provider, boolean fromClient) {
        synchronized (mProviderMap) {
            handleUnstableProviderDiedLocked(provider, fromClient);
        }
    }

    final void handleUnstableProviderDiedLocked(IBinder provider, boolean fromClient) {
        ProviderRefCount prc = mProviderRefCountMap.get(provider);
        if (prc != null) {
            if (DEBUG_PROVIDER) Slog.v(TAG, "Cleaning up dead provider "
                    + provider + " " + prc.holder.info.name);
            mProviderRefCountMap.remove(provider);
            for (int i=mProviderMap.size()-1; i>=0; i--) {
                ProviderClientRecord pr = mProviderMap.valueAt(i);
                if (pr != null && pr.mProvider.asBinder() == provider) {
                    Slog.i(TAG, "Removing dead content provider:" + pr.mProvider.toString());
                    mProviderMap.removeAt(i);
                }
            }

            if (fromClient) {
                // We found out about this due to execution in our client
                // code.  Tell the activity manager about it now, to ensure
                // that the next time we go to do anything with the provider
                // it knows it is dead (so we don't race with its death
                // notification).
                try {
                    ActivityManagerNative.getDefault().unstableProviderDied(
                            prc.holder.connection);
                } catch (RemoteException e) {
                    //do nothing content provider object is dead any way
                }
            }
        }
    }

    final void appNotRespondingViaProvider(IBinder provider) {
        synchronized (mProviderMap) {
            ProviderRefCount prc = mProviderRefCountMap.get(provider);
            if (prc != null) {
                try {
                    ActivityManagerNative.getDefault()
                            .appNotRespondingViaProvider(prc.holder.connection);
                } catch (RemoteException e) {
                    throw e.rethrowFromSystemServer();
                }
            }
        }
    }

    private ProviderClientRecord installProviderAuthoritiesLocked(IContentProvider provider,
            ContentProvider localProvider, IActivityManager.ContentProviderHolder holder) {
        final String auths[] = holder.info.authority.split(";");
        final int userId = UserHandle.getUserId(holder.info.applicationInfo.uid);

        final ProviderClientRecord pcr = new ProviderClientRecord(
                auths, provider, localProvider, holder);
        for (String auth : auths) {
            final ProviderKey key = new ProviderKey(auth, userId);
            final ProviderClientRecord existing = mProviderMap.get(key);
            if (existing != null) {
                Slog.w(TAG, "Content provider " + pcr.mHolder.info.name
                        + " already published as " + auth);
            } else {
                mProviderMap.put(key, pcr);
            }
        }
        return pcr;
    }

    /**
     * Installs the provider.
     *
     * Providers that are local to the process or that come from the system server
     * may be installed permanently which is indicated by setting noReleaseNeeded to true.
     * Other remote providers are reference counted.  The initial reference count
     * for all reference counted providers is one.  Providers that are not reference
     * counted do not have a reference count (at all).
     *
     * This method detects when a provider has already been installed.  When this happens,
     * it increments the reference count of the existing provider (if appropriate)
     * and returns the existing provider.  This can happen due to concurrent
     * attempts to acquire the same provider.
     */
    private IActivityManager.ContentProviderHolder installProvider(Context context,
            IActivityManager.ContentProviderHolder holder, ProviderInfo info,
            boolean noisy, boolean noReleaseNeeded, boolean stable) {
        ContentProvider localProvider = null;
        IContentProvider provider;
        if (holder == null || holder.provider == null) {
            if (DEBUG_PROVIDER || noisy) {
                Slog.d(TAG, "Loading provider " + info.authority + ": "
                        + info.name);
            }
            Context c = null;
            ApplicationInfo ai = info.applicationInfo;
            if (context.getPackageName().equals(ai.packageName)) {
                c = context;
            } else if (mInitialApplication != null &&
                    mInitialApplication.getPackageName().equals(ai.packageName)) {
                c = mInitialApplication;
            } else {
                try {
                    c = context.createPackageContext(ai.packageName,
                            Context.CONTEXT_INCLUDE_CODE);
                } catch (PackageManager.NameNotFoundException e) {
                    // Ignore
                }
            }
            if (c == null) {
                Slog.w(TAG, "Unable to get context for package " +
                      ai.packageName +
                      " while loading content provider " +
                      info.name);
                return null;
            }
            try {
                final java.lang.ClassLoader cl = c.getClassLoader();
                localProvider = (ContentProvider)cl.
                    loadClass(info.name).newInstance();
                provider = localProvider.getIContentProvider();
                if (provider == null) {
                    Slog.e(TAG, "Failed to instantiate class " +
                          info.name + " from sourceDir " +
                          info.applicationInfo.sourceDir);
                    return null;
                }
                if (DEBUG_PROVIDER) Slog.v(
                    TAG, "Instantiating local provider " + info.name);
                // XXX Need to create the correct context for this provider.
                localProvider.attachInfo(c, info);
            } catch (java.lang.Exception e) {
                if (!mInstrumentation.onException(null, e)) {
                    throw new RuntimeException(
                            "Unable to get provider " + info.name
                            + ": " + e.toString(), e);
                }
                return null;
            }
        } else {
            provider = holder.provider;
            if (DEBUG_PROVIDER) Slog.v(TAG, "Installing external provider " + info.authority + ": "
                    + info.name);
        }

        IActivityManager.ContentProviderHolder retHolder;

        synchronized (mProviderMap) {
            if (DEBUG_PROVIDER) Slog.v(TAG, "Checking to add " + provider
                    + " / " + info.name);
            IBinder jBinder = provider.asBinder();
            if (localProvider != null) {
                ComponentName cname = new ComponentName(info.packageName, info.name);
                ProviderClientRecord pr = mLocalProvidersByName.get(cname);
                if (pr != null) {
                    if (DEBUG_PROVIDER) {
                        Slog.v(TAG, "installProvider: lost the race, "
                                + "using existing local provider");
                    }
                    provider = pr.mProvider;
                } else {
                    holder = new IActivityManager.ContentProviderHolder(info);
                    holder.provider = provider;
                    holder.noReleaseNeeded = true;
                    pr = installProviderAuthoritiesLocked(provider, localProvider, holder);
                    mLocalProviders.put(jBinder, pr);
                    mLocalProvidersByName.put(cname, pr);
                }
                retHolder = pr.mHolder;
            } else {
                ProviderRefCount prc = mProviderRefCountMap.get(jBinder);
                if (prc != null) {
                    if (DEBUG_PROVIDER) {
                        Slog.v(TAG, "installProvider: lost the race, updating ref count");
                    }
                    // We need to transfer our new reference to the existing
                    // ref count, releasing the old one...  but only if
                    // release is needed (that is, it is not running in the
                    // system process).
                    if (!noReleaseNeeded) {
                        incProviderRefLocked(prc, stable);
                        try {
                            ActivityManagerNative.getDefault().removeContentProvider(
                                    holder.connection, stable);
                        } catch (RemoteException e) {
                            //do nothing content provider object is dead any way
                        }
                    }
                } else {
                    ProviderClientRecord client = installProviderAuthoritiesLocked(
                            provider, localProvider, holder);
                    if (noReleaseNeeded) {
                        prc = new ProviderRefCount(holder, client, 1000, 1000);
                    } else {
                        prc = stable
                                ? new ProviderRefCount(holder, client, 1, 0)
                                : new ProviderRefCount(holder, client, 0, 1);
                    }
                    mProviderRefCountMap.put(jBinder, prc);
                }
                retHolder = prc.holder;
            }
        }

        return retHolder;
    }

    private void attach(boolean system) {
        sCurrentActivityThread = this;
        mSystemThread = system;
        if (!system) {
            ViewRootImpl.addFirstDrawHandler(new Runnable() {
                @Override
                public void run() {
                    ensureJitEnabled();
                }
            });
            android.ddm.DdmHandleAppName.setAppName("<pre-initialized>",
                                                    UserHandle.myUserId());
            RuntimeInit.setApplicationObject(mAppThread.asBinder());
            final IActivityManager mgr = ActivityManagerNative.getDefault();
            try {
                mgr.attachApplication(mAppThread);
            } catch (RemoteException ex) {
                throw ex.rethrowFromSystemServer();
            }
            // Watch for getting close to heap limit.
            BinderInternal.addGcWatcher(new Runnable() {
                @Override public void run() {
                    if (!mSomeActivitiesChanged) {
                        return;
                    }
                    Runtime runtime = Runtime.getRuntime();
                    long dalvikMax = runtime.maxMemory();
                    long dalvikUsed = runtime.totalMemory() - runtime.freeMemory();
                    if (dalvikUsed > ((3*dalvikMax)/4)) {
                        if (DEBUG_MEMORY_TRIM) Slog.d(TAG, "Dalvik max=" + (dalvikMax/1024)
                                + " total=" + (runtime.totalMemory()/1024)
                                + " used=" + (dalvikUsed/1024));
                        mSomeActivitiesChanged = false;
                        try {
                            mgr.releaseSomeActivities(mAppThread);
                        } catch (RemoteException e) {
                            throw e.rethrowFromSystemServer();
                        }
                    }
                }
            });
        } else {
            // Don't set application object here -- if the system crashes,
            // we can't display an alert, we just want to die die die.
            android.ddm.DdmHandleAppName.setAppName("system_process",
                    UserHandle.myUserId());
            try {
                mInstrumentation = new Instrumentation();
                ContextImpl context = ContextImpl.createAppContext(
                        this, getSystemContext().mPackageInfo);
                mInitialApplication = context.mPackageInfo.makeApplication(true, null);
                mInitialApplication.onCreate();
            } catch (Exception e) {
                throw new RuntimeException(
                        "Unable to instantiate Application():" + e.toString(), e);
            }
        }

        // add dropbox logging to libcore
        DropBox.setReporter(new DropBoxReporter());

        ViewRootImpl.addConfigCallback(new ComponentCallbacks2() {
            @Override
            public void onConfigurationChanged(Configuration newConfig) {
                synchronized (mResourcesManager) {
                    // We need to apply this change to the resources
                    // immediately, because upon returning the view
                    // hierarchy will be informed about it.
                    if (mResourcesManager.applyConfigurationToResourcesLocked(newConfig, null)) {
                        updateLocaleListFromAppContext(mInitialApplication.getApplicationContext(),
                                mResourcesManager.getConfiguration().getLocales());

                        // This actually changed the resources!  Tell
                        // everyone about it.
                        if (mPendingConfiguration == null ||
                                mPendingConfiguration.isOtherSeqNewer(newConfig)) {
                            mPendingConfiguration = newConfig;

                            sendMessage(H.CONFIGURATION_CHANGED, newConfig);
                        }
                    }
                }
            }
            @Override
            public void onLowMemory() {
            }
            @Override
            public void onTrimMemory(int level) {
            }
        });
    }

    public static ActivityThread systemMain() {
        // The system process on low-memory devices do not get to use hardware
        // accelerated drawing, since this can add too much overhead to the
        // process.
        if (!ActivityManager.isHighEndGfx()) {
            ThreadedRenderer.disable(true);
        } else {
            ThreadedRenderer.enableForegroundTrimming();
        }
        ActivityThread thread = new ActivityThread();
        thread.attach(true);
        return thread;
    }

    public final void installSystemProviders(List<ProviderInfo> providers) {
        if (providers != null) {
            installContentProviders(mInitialApplication, providers);
        }
    }

    public int getIntCoreSetting(String key, int defaultValue) {
        synchronized (mResourcesManager) {
            if (mCoreSettings != null) {
                return mCoreSettings.getInt(key, defaultValue);
            }
            return defaultValue;
        }
    }

    private static class EventLoggingReporter implements EventLogger.Reporter {
        @Override
        public void report (int code, Object... list) {
            EventLog.writeEvent(code, list);
        }
    }

    private class DropBoxReporter implements DropBox.Reporter {

        private DropBoxManager dropBox;

        public DropBoxReporter() {}

        @Override
        public void addData(String tag, byte[] data, int flags) {
            ensureInitialized();
            dropBox.addData(tag, data, flags);
        }

        @Override
        public void addText(String tag, String data) {
            ensureInitialized();
            dropBox.addText(tag, data);
        }

        private synchronized void ensureInitialized() {
            if (dropBox == null) {
                dropBox = (DropBoxManager) getSystemContext().getSystemService(Context.DROPBOX_SERVICE);
            }
        }
    }

    public static void main(String[] args) {
        Trace.traceBegin(Trace.TRACE_TAG_ACTIVITY_MANAGER, "ActivityThreadMain");
        SamplingProfilerIntegration.start();

        // CloseGuard defaults to true and can be quite spammy.  We
        // disable it here, but selectively enable it later (via
        // StrictMode) on debug builds, but using DropBox, not logs.
        CloseGuard.setEnabled(false);

        Environment.initForCurrentUser();

        // Set the reporter for event logging in libcore
        EventLogger.setReporter(new EventLoggingReporter());

        // Make sure TrustedCertificateStore looks in the right place for CA certificates
        final File configDir = Environment.getUserConfigDirectory(UserHandle.myUserId());
        TrustedCertificateStore.setDefaultUserDirectory(configDir);

        Process.setArgV0("<pre-initialized>");

        Looper.prepareMainLooper();

        ActivityThread thread = new ActivityThread();
        thread.attach(false);

        if (sMainThreadHandler == null) {
            sMainThreadHandler = thread.getHandler();
        }

        if (false) {
            Looper.myLooper().setMessageLogging(new
                    LogPrinter(Log.DEBUG, "ActivityThread"));
        }

        // End of event ActivityThreadMain.
        Trace.traceEnd(Trace.TRACE_TAG_ACTIVITY_MANAGER);
        Looper.loop();

        throw new RuntimeException("Main thread loop unexpectedly exited");
    }
}
