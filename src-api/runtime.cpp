#include "runtime.hpp"
#include "scheduler.hpp"
#include <plog/Log.h>
#include <deque>
#include <queue>
#include <mutex>

#include "ipc.h"
#include "dag_parse.hpp"
#include <sys/stat.h>
#include <cstdio>
#include <list>
#include <thread>

#include <random>
#include <sched.h>
#include <pthread.h>
#include <cstdarg>
#include <climits>
#include <cinttypes>

// LUT array
int LUT_array[api_types::NUM_API_TYPES] = {-1};

// Initialize ready and poison queues
std::deque<task_nodes *> ready_queue;   // INFO: Use for storing DASH_API tasks only <- APPs will push DASH_API calls into this queue
std::deque<task_nodes *> poison_queue;  // NOTE: enqueue_task will push poison-pills only into this queue
pthread_mutex_t ready_queue_mutex;
pthread_mutex_t poison_queue_mutex;

std::map <pthread_t, cedr_app *> app_thread_map;
pthread_mutex_t app_thread_map_mutex;

void* dash_functions [api_types::NUM_API_TYPES][precision_types::NUM_PRECISION_TYPES][resource_type::NUM_RESOURCE_TYPES] = {NULL};
std::list<std::pair<std::string, void*>> dash_dlhandles = {};

extern "C" void enqueue_kernel(const char* kernel_name, const char* precision_name, uint32_t n_vargs, ...) {
  // Check which type of DASH_API call has been made (if-else)
  // For the selected API call, create list of arguments and push in ready_queue
  // If the poison-pill call has been selected, then push it in poison_queue;

  std::string kernel_str(kernel_name);
  std::string precision_str(precision_name);

  // Special case :0
  // Poison pills are special types of nodes that lack corresponding API calls or API-type enums
  if (kernel_str == "POISON_PILL") {
    LOG_INFO << "I am injecting a poison pill to tell the host thread that I'm done executing";

    task_nodes* new_node = (task_nodes*) calloc(1, sizeof(task_nodes));
    new_node->task_name = "POISON PILL";
    new_node->parent_app_pthread = pthread_self();
    pthread_mutex_lock(&app_thread_map_mutex);
    new_node->app_pnt = app_thread_map[pthread_self()];
    pthread_mutex_unlock(&app_thread_map_mutex);

    pthread_mutex_lock(&poison_queue_mutex);
    poison_queue.push_back(new_node);
    pthread_mutex_unlock(&poison_queue_mutex);
    LOG_INFO <<"I have pushed the poison pill onto the task list";

    return;
  }

  // Other special case :0
  // The user requested running a kernel that we don't recognize
  if (api_types_map.find(kernel_str) == api_types_map.end()) {
    LOG_WARNING << "Received a request to enqueue a kernel of type \'" << kernel_str << "\', but that kernel isn't recognized by the runtime!";
    return;
  }

  api_types api = api_types_map.at(kernel_str);
  precision_types precision = precision_types_map.at(precision_str);

  va_list args;
  va_start(args, n_vargs);

  LOG_INFO << "I am enqueueing a new \'" << kernel_str << "\' task";

  task_nodes* new_node = (task_nodes*) calloc(1, sizeof(task_nodes));
  new_node->task_name = kernel_str;
  new_node->task_type = api;
  
  for (int i = 0; i < n_vargs; i++) {
    // The last varg needs to be our completion barrier struct
    if (i == n_vargs - 1) {
      new_node->kernel_barrier = va_arg(args, cedr_barrier_t*);
    } 
    // Otherwise, just push all the void* args into our list
    else {
      new_node->args.push_back(va_arg(args, void*));
    }
  }
  size_t input_size = *static_cast<size_t*>(new_node->args.at(2));
  bool is_fft = *static_cast<bool*>(new_node->args.at(3));
  new_node->is_fft = is_fft; // Default value, will be set to true if this is a FFT task
  new_node->input_size = input_size; // Default value, will be set to the size of the input data

  for (int resource = 0; resource < resource_type::NUM_RESOURCE_TYPES; resource++) {
    // If we have an implementation for kernel "kernel_str" on resource "resource", ...
    if (dash_functions[api][precision][resource] != nullptr) {
      // Add it to our array of function pointers
      new_node->run_funcs[resource] = (void*) dash_functions[api][precision][resource];
      new_node->supported_resources[resource] = true;
    }
  }

  IF_LOG(plog::debug) {
    LOG_DEBUG << "Supported resources for task \'" << kernel_str << "\' are";
    for (const auto resourceType : new_node->supported_resources){
      LOG_DEBUG << resource_type_names[(uint8_t) resourceType];
    }
  }

  new_node->parent_app_pthread = pthread_self();
  //TODO: Does app_thread_map require a mutex?
  pthread_mutex_lock(&app_thread_map_mutex);
  new_node->app_pnt = app_thread_map[pthread_self()];
  pthread_mutex_unlock(&app_thread_map_mutex);
  new_node->task_id = new_node->app_pnt->task_count;  new_node->app_pnt->task_count++;

  // Push this node onto the ready queue
  // Note: this would be a GREAT place for a lock-free multi-producer queue
  // Otherwise, every application trying to push in new work is going to get stuck waiting for some eventual ready queue mutex
  pthread_mutex_lock(&ready_queue_mutex);
  ready_queue.push_back(new_node);
  pthread_mutex_unlock(&ready_queue_mutex);
  LOG_INFO << "I have finished initializing my \'" << kernel_str << "\' node and pushed it onto the task list";
}

void nk_thread_func(void* runfunc) {
  // Cast our voidptr argument to be a function pointer #justCThings
  // int main(int argc, char** argv, char** envp)
  int (*libmain)(int, char**, char**) = (int(*)(int, char**, char**)) runfunc;
  // Call the library's main
  const char* argv = "cedr_app";
  (*libmain)(1, (char**) &argv, nullptr);
  // Once it's complete, enqueue a poison pill to signal the runtime
  enqueue_kernel("POISON_PILL", precision_type_names[0], 0);
}

void parseAPIImplementations(ConfigManager &cedr_config) {
  std::list<std::string> dash_so_paths = cedr_config.getDashBinaryPaths();

  for (auto& dash_so_path : dash_so_paths) {
    LOG_INFO << "Attempting to open dash binary at " << dash_so_path;

    void *dash_dlhandle = dlopen(dash_so_path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
    if(!dash_dlhandle) {
      LOG_ERROR << "Failed to open dash binary";
      fprintf(stderr, "Failed to open DASH library at: %s. Please ensure the shared object is present or adjust your configuration file.\nFurther details are provided below:\n%s\n", dash_so_path.c_str(), dlerror());
      exit(1);
    }

    dash_dlhandles.push_back({dash_so_path, dash_dlhandle});
    LOG_INFO << "Dash binary at " << dash_so_path << " was opened successfully.";
  }

  LOG_INFO << "All dash binaries opened successfully. Scanning for API implementations";
  for (int api = 0; api < api_types::NUM_API_TYPES; api++) {
    for (int precision = 0; precision < precision_types::NUM_PRECISION_TYPES; precision++) {
      for (int resource = 0; resource < resource_type::NUM_RESOURCE_TYPES; resource++) {
        std::string api_name(api_type_names[api]);
        std::string precision_name(precision_type_names[precision]);
        std::string resource_name(resource_type_names[resource]);

        std::string expected_api_function = api_name + "_" + precision_name + "_" + resource_name;
        LOG_DEBUG << "Attempting to find API implementation \'" << expected_api_function << "\'";
        for (auto& handle_pair : dash_dlhandles) {
          std::string dash_so_path = handle_pair.first;
          void* dash_dlhandle = handle_pair.second;
          void* symbol_ptr = dlsym(dash_dlhandle, expected_api_function.c_str());
          if (symbol_ptr == NULL) {
            LOG_DEBUG << "Unable to locate API implementation \'" << expected_api_function << "\'" << " within binary \'" << dash_so_path << "\'";
          } else {
            LOG_INFO << "Located API implementation \'" << expected_api_function << "\'" << "\'" << " within binary \'" << dash_so_path << "\'";
            if (dash_functions[api][precision][resource] != NULL) {
              LOG_WARNING << "We've previously located an implementation of this API, but I'm going to overwrite it now with this newer implementation.";
            } 
            dash_functions[api][precision][resource] = symbol_ptr;
          }
        }
      }
    }
  }
}

void closeAPIImplementations(void) {
  for (auto& handle_pair : dash_dlhandles) {
    dlclose(handle_pair.second);
  }
}

void initializeLUT(ConfigManager &cedr_config) {
  for (int api = 0; api < api_types::NUM_API_TYPES; api++) {
    std::pair<resource_type, uint64_t> min_resource = {resource_type::cpu, std::numeric_limits<uint64_t>::max()};
    for (int res = 0; res < resource_type::NUM_RESOURCE_TYPES; res++) {
      uint64_t exec_time = cedr_config.getDashExecTime((api_types) api, (resource_type) res);
      if (exec_time < min_resource.second) {
        min_resource = { (resource_type) res, exec_time };
      }
    }
    // Simple LUT assignment, assign best resource for current task, might need sophistication to resolve conflict 
    LUT_array[api] = min_resource.first;
  }
}

void launchDaemonRuntime(ConfigManager &cedr_config, pthread_t *resource_handle, worker_thread *hardware_thread_handle) {
  LOG_DEBUG << "Beginning execution of Performance mode";
  
  struct timespec real_current_time {};

  struct timespec sleep_time {};
  sleep_time.tv_sec = 0;
  sleep_time.tv_nsec = 2000;
  std::list<struct struct_logging> stream_timing_log;

  // Schedule timing
  struct timespec schedule_starttime {};
  struct timespec schedule_stoptime {};
  std::list<struct struct_schedlogging> sched_log;
  std::list<struct struct_applogging> app_log;

  struct timespec ipc_time_start {}; 
  struct timespec ipc_time_end{};

  LOG_INFO << "Initializing the shared memory region in the server to communicate with the clients";
  struct process_ipc_struct ipc_cmd {};
  ipc_cmd.message = (ipc_message_struct *) calloc(1, sizeof(ipc_message_struct));
  initialize_sh_mem_server(&ipc_cmd);

  // Scan our provided API binary for implementations that can be used for each of our various APIs
  parseAPIImplementations(cedr_config);
  initializeLUT(cedr_config);

  // Construct a min heap-style priority queue where the application with the
  // lowest arrival time is always on top
  auto app_comparator = [](cedr_app *first, cedr_app *second) { return second->arrival_time < first->arrival_time; };
  std::priority_queue<cedr_app *, std::vector<cedr_app *>, decltype(app_comparator)> unarrived_apps(app_comparator);

  std::list<cedr_app *> arrived_nonstreaming_apps;
  std::list<cedr_app *> completed_nonstreaming_apps;

  int appNum = 0;

  size_t completed_task_queue_length = 0;

  uint32_t free_resource_count = cedr_config.getTotalResources();

  pthread_mutex_init(&ready_queue_mutex, NULL);
  pthread_mutex_init(&poison_queue_mutex, NULL);

  std::map<std::string, void *> sharedObjectMap;    // NOTE: May not be needed
  std::map<std::string, cedr_app *> applicationMap;

  // Initialize exponential distribution and random number generator
  std::default_random_engine generator(cedr_config.getRandomSeed());
  std::exponential_distribution<double> distribution(1.0f);

  // Create directory for the daemon process to capture the execution time for the submitted jobs
  struct stat info {};
  std::string log_path;
  if (stat("./log_dir/", &info) != 0) {
    mkdir("./log_dir/", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    chmod("./log_dir/", S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
  }
  {
    int run_id = 0;
    std::string run_folder_name = "./log_dir/experiment";
    while (true) {
      if (stat((run_folder_name + std::to_string(run_id)).c_str(), &info) != 0) {
        mkdir((run_folder_name + std::to_string(run_id)).c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        chmod((run_folder_name + std::to_string(run_id)).c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        log_path = run_folder_name + std::to_string(run_id);
        break;
      }
      run_id++;
    }
    LOG_INFO << "Run logs will be created in : " << log_path;
  }


  uint64_t emul_time;
  bool receivedFirstSubDag = false;
  // Bind all the parameters to this lambda from their outside references such that, when we call the lambda, we don't
  // need a million arguments
  auto shouldExitEarly = [&cedr_config, &receivedFirstSubDag, &unarrived_apps, &arrived_nonstreaming_apps]() {
    return (cedr_config.getExitWhenIdle() && receivedFirstSubDag && unarrived_apps.empty() && arrived_nonstreaming_apps.empty());
  };

  // Build a CPU set that will allow us to assign all NK threads to run on cores other than where CEDR is
  cpu_set_t non_kernel_cpuset;
  const unsigned int processor_count = std::thread::hardware_concurrency();
  CPU_ZERO(&non_kernel_cpuset);
  // Core assignments for application threads
  //for (int cpu_id = processor_count-2; cpu_id >= 3; cpu_id--) { //little
  //for (int cpu_id = processor_count-2; cpu_id >= 0; cpu_id--) { //3big or 4big (app threads are handled by all CPUs)

  // Only CPU resources handle the threads
  //for (int cpu_id = processor_count-2; cpu_id >= 1; cpu_id--) { //3big cores
  // for (int cpu_id = processor_count-2; cpu_id >= 2; cpu_id--) { //4big cores
  //   CPU_SET(cpu_id, &non_kernel_cpuset);
  //   LOG_VERBOSE << "Including CPU " << cpu_id << " to available core list that applications can use for NK execution";
  // }
  for (int cpu_id = processor_count-1; cpu_id >= 0; cpu_id--) { // All cores
    CPU_SET(cpu_id, &non_kernel_cpuset);
    LOG_VERBOSE << "Including CPU " << cpu_id << " to available core list that applications can use for NK execution";
  }


  // Variable to keep track of killed/resolved applications
  int resolved_app_count = 0; 
  int killed_app_count = 0;
  // Begin execution
  LOG_INFO << "Indefinite run starting";
  while (true) {
    sh_mem_server_process_cmd(&ipc_cmd);
    if (ipc_cmd.cmd_type == SH_OBJ) { // Command for new application received
      // clock_gettime(CLOCK_MONOTONIC_RAW, &ipc_time_start);
      char soPaths[IPC_MAX_APPS][IPC_MAX_PATH_LEN];
      uint64_t num_instances[IPC_MAX_APPS];
      uint64_t periods[IPC_MAX_APPS];
      memcpy(soPaths, ipc_cmd.message->path_to_so, sizeof(soPaths));
      memcpy(num_instances, ipc_cmd.message->num_instances, sizeof(num_instances));
      memcpy(periods, ipc_cmd.message->periodicity, sizeof(periods));

      receivedFirstSubDag = true;

      for (auto appIdx = 0; appIdx < IPC_MAX_APPS; appIdx++) {
        LOG_VERBOSE << "Processing Application slot number " << appIdx << " from the IPC cmd";
        if (strlen(soPaths[appIdx]) == 0) {
          LOG_VERBOSE << "This slot was empty, assuming we're done injecting DAGs";
          break;
        }
        const std::string pathToSO(soPaths[appIdx]);
        const uint64_t num_inst = num_instances[appIdx];
        const uint64_t period = uint64_t(periods[appIdx]);       
        if (num_inst == 0) {
          LOG_WARNING << "APP " << pathToSO << " was injected, but 0 instances were requested, so I'm skipping it";
          continue;
        }

        std::string appName;
        // If we don't have any '/' in the string, assume the DAG name is the full path name
        if (pathToSO.find('/') == std::string::npos) {
          appName = pathToSO;
        } else {
          appName = pathToSO.substr(pathToSO.find_last_of('/') + 1);
        }
        LOG_INFO << "Received application: " << appName << ". Will attempt to inject " << num_inst << " instances of it with a period of " << period << " microseconds";
        cedr_app *prototypeAppPtr;
        auto appIsPresent = applicationMap.find(appName);
        if (appIsPresent == applicationMap.end()) {
          LOG_DEBUG << appName << " is not cached. Parsing and caching";
          prototypeAppPtr = parse_binary(pathToSO, sharedObjectMap);
          if (prototypeAppPtr != nullptr) {
            applicationMap[appName] = prototypeAppPtr;
          }
          else {
            LOG_ERROR << "Failed to open application shared object due to previous errors! Ignoring application " << appName;
          }
        } else {
          LOG_DEBUG << appName << " was cached, initializing with existing prototype";
          prototypeAppPtr = appIsPresent->second;
        }

        uint64_t new_apps_instantiated = 0;
        cedr_app *new_app;
        uint64_t accum_period = period;
        if (period > 0) {
          distribution = std::exponential_distribution<double>(1.0f / period);
        }
        while (prototypeAppPtr && (new_apps_instantiated < num_inst)) {
          emul_time = cedrGetTime(hardware_thread_handle[0].time_per_cycle);
          new_app = (cedr_app *)calloc(1, sizeof(cedr_app));
          *new_app = *prototypeAppPtr;
          new_app->app_id = appNum;
          new_app->task_count = 0;
          new_app->is_running = false;
          new_app->arrival_time = emul_time + accum_period * US2NANOSEC;
          if (cedr_config.getFixedPeriodicInjection()) {
            accum_period += period;
          } else {
            accum_period += distribution(generator);
          }
          appNum++;
          unarrived_apps.push(new_app);
          new_apps_instantiated++;
        }
      }

      ipc_cmd.cmd_type = NOPE;
      // clock_gettime(CLOCK_MONOTONIC_RAW, &ipc_time_end);
      // LOG_WARNING << "Injecting new applications took " << (ipc_time_end.tv_sec - ipc_time_start.tv_sec) *
      // SEC2NANOSEC + (ipc_time_end.tv_nsec - ipc_time_start.tv_nsec) << " nanoseconds";

    } else if (ipc_cmd.cmd_type == SERVER_EXIT || shouldExitEarly()) {
        LOG_INFO << "Command to terminate daemon process received";
        // Wait for HW threads termination
        for (int i = 0; i < cedr_config.getTotalResources(); i++) {
          while (true){
            pthread_mutex_lock(&(hardware_thread_handle[i].resource_mutex));
            if (hardware_thread_handle[i].resource_state == 0) {
              hardware_thread_handle[i].resource_state = 3;
              pthread_mutex_unlock(&(hardware_thread_handle[i].resource_mutex));
              break;
            }
            pthread_mutex_unlock(&(hardware_thread_handle[i].resource_mutex));
          }
        }

      if (arrived_nonstreaming_apps.size() != 0) {
      	LOG_INFO << "Remaining applications running are " << arrived_nonstreaming_apps.size();
      }
      // Join the app threads that have completed
      pthread_mutex_lock(&poison_queue_mutex);
      for (auto task : poison_queue){
        pthread_join(task->parent_app_pthread, NULL);
        // TODO: Mark end time as the push time of poison task instead of current_time.
        task->app_pnt->finish_time = cedrGetTime(hardware_thread_handle[0].time_per_cycle);

        resolved_app_count++;
        arrived_nonstreaming_apps.remove(task->app_pnt);
        
        // Removing corresponding pthread map of erased_app
        pthread_mutex_lock(&app_thread_map_mutex);
        auto map_remove = app_thread_map.find(task->app_pnt->app_pthread);
        app_thread_map.erase(map_remove);   
        pthread_mutex_unlock(&app_thread_map_mutex);
        
        completed_nonstreaming_apps.push_front(task->app_pnt);
        LOG_INFO << "Joined pthread of application " << task->app_pnt->app_name << "_" << task->app_pnt->app_id;
        }
      pthread_mutex_unlock(&poison_queue_mutex);

      // Kick out forcibly the arrived_nonstreaming_apps threads that are still running
      // INFO: These applications are not logged
      if (!arrived_nonstreaming_apps.size()){
        LOG_INFO << "At the end, remaining unresolved app number is " << arrived_nonstreaming_apps.size();
        for (auto running_app : arrived_nonstreaming_apps){
          pthread_cancel(running_app->app_pthread);
          killed_app_count++;
          pthread_mutex_lock(&app_thread_map_mutex);
          app_thread_map.erase(running_app->app_pthread);
          pthread_mutex_unlock(&app_thread_map_mutex);
          arrived_nonstreaming_apps.remove(running_app);
        }
      }

      ipc_cmd.cmd_type = NOPE;
      LOG_INFO << "Exit command initiated. Terminating runtime while-loop";
      break;
    }

    // Push the heads nodes of the the newly arrived applications on the ready_queue  
    emul_time = cedrGetTime(hardware_thread_handle[0].time_per_cycle); 
   
    while (!unarrived_apps.empty() && (unarrived_apps.top()->arrival_time <= emul_time)) {
      cedr_app *app_inst = unarrived_apps.top();
      LOG_DEBUG << "Application: (" << app_inst->app_name << ", " << app_inst->app_id << ") arrived at time " << emul_time;
      unarrived_apps.pop();

      app_inst->arrival_time = emul_time;
      // numParallelApps++;
      arrived_nonstreaming_apps.push_back(app_inst);
      //LOG_INFO << "Unarrived apps size: " << unarrived_apps.size();
      //LOG_INFO << "Arrived Non-streaming app queue size " << arrived_nonstreaming_apps.size();
    }

    for (auto app : arrived_nonstreaming_apps){
      // TODO: Pass proper data type to pthread_create as function handle
      if (!app->is_running) {
        LOG_INFO << "[DEBUG_SETAFFINITY] Trying to set the first pthread affinity";
        pthread_attr_t non_kernel_attr;
        pthread_attr_init(&non_kernel_attr);
        //int affinity_check = pthread_attr_setaffinity_np(&non_kernel_attr, sizeof(cpu_set_t) * (cedr_config.getResourceArray()[resource_type::cpu] - 1),
        //                                                   &non_kernel_cpuset);
        int affinity_check = pthread_attr_setaffinity_np(&non_kernel_attr, sizeof(cpu_set_t),
                                                           &non_kernel_cpuset);
        //int affinity_check = pthread_attr_setaffinity_np(&non_kernel_attr, non_kernel_cpusize, non_kernel_cpuset);
        if (affinity_check != 0){
          LOG_ERROR << "Failed to set cpu affinity for nk thread. Returned value from pthread_setaffinity_np is " << affinity_check;
          exit(1);
        }
        pthread_attr_setinheritsched(&non_kernel_attr, PTHREAD_EXPLICIT_SCHED);
        int setschedpolicy_check = pthread_attr_setschedpolicy(&non_kernel_attr, SCHED_OTHER);
        if (setschedpolicy_check != 0){
          LOG_ERROR << "Failed to set scheduling policy for application " << app->app_name << " with app_id " << app->app_id;
        }
        sched_param non_kernel_schedparams;
        non_kernel_schedparams.sched_priority = 0;
        pthread_attr_setschedparam(&non_kernel_attr, &non_kernel_schedparams);

        // NOTE: Finally creating thread
        uint64_t clock_vir = cedrGetTime(hardware_thread_handle[0].time_per_cycle);          
        pthread_create(&(app->app_pthread), &non_kernel_attr, (void *(*)(void *)) nk_thread_func, (void*) app->main_func_handle);
        app->start_time = clock_vir;
        app->is_running = true;
        LOG_INFO << "Thread for application " << app->app_name << " launched!";
        pthread_mutex_lock(&app_thread_map_mutex);
        app_thread_map[app->app_pthread] = app; // NEW: Store application structs indexed by their main thread
        pthread_mutex_unlock(&app_thread_map_mutex);
      }
    }

    pthread_mutex_lock(&ready_queue_mutex);     // TODO: Optimize so that ready_queue doesn't remain locked for the entire performScheduling routine
      if (!ready_queue.empty()) {
        LOG_INFO << "Scheduling round found " << ready_queue.size() << " tasks in ready task queue!";
        struct struct_schedlogging schedlog_obj {};
        schedlog_obj.ready_queue_size = ready_queue.size();
        uint64_t clock_vir0 = cedrGetTime(hardware_thread_handle[0].time_per_cycle);
        performScheduling(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
        uint64_t clock_vir1 = cedrGetTime(hardware_thread_handle[0].time_per_cycle);    
        schedlog_obj.start = clock_vir0;
        schedlog_obj.end = clock_vir1;
        schedlog_obj.scheduling_overhead = (clock_vir1 - clock_vir0);
        LOG_INFO << "scheduling_overhead: " << (clock_vir1 - clock_vir0);
        sched_log.push_back(schedlog_obj);
        LOG_DEBUG << "Ready queue has " << ready_queue.size() << " number of tasks after launching application threads!";
        pthread_mutex_unlock(&ready_queue_mutex);
    } else{
        pthread_mutex_unlock(&ready_queue_mutex);
    }

    // Remove completed applications from arrived_nonstreaming_apps queue and 
    // push to the completed_nonstreaming_apps queue for logging purposes
    pthread_mutex_lock(&poison_queue_mutex);
    for (auto poison_task = poison_queue.begin(), poison_task_end = poison_queue.end(); poison_task != poison_task_end;){
      auto poison_task_erase = poison_task; poison_task++;
      const auto erased_app = (*poison_task_erase)->app_pnt;

      int J = pthread_join((*poison_task_erase)->parent_app_pthread, NULL);
      erased_app->finish_time = cedrGetTime(hardware_thread_handle[0].time_per_cycle);
      
      if (J==0) {
        LOG_VERBOSE << "Joined application (non-kernel) thread with id " << erased_app->app_pthread;
        resolved_app_count++;
        LOG_INFO << "Number of resolved applications: " << resolved_app_count << "; Number of killed applications: " << killed_app_count;
      } else {
        // TODO: Instead of killing CEDR, can we use pthread_cancel to force quit? 
	      // In that case CEDR won't need to exit.
        LOG_ERROR << "Failed to join thread with id " << erased_app->app_pthread;
        exit(1);
      }
      arrived_nonstreaming_apps.remove(erased_app);
      // Removing corresponding pthread map of erased_app
      pthread_mutex_lock(&app_thread_map_mutex);
      auto map_remove = app_thread_map.find(erased_app->app_pthread);
      app_thread_map.erase(map_remove);
      pthread_mutex_unlock(&app_thread_map_mutex);

      completed_nonstreaming_apps.push_front(erased_app);
      free(*poison_task_erase);
      poison_queue.erase(poison_task_erase);
    }
    pthread_mutex_unlock(&poison_queue_mutex);
    // pthread_yield();
  }

  for (int i = 0; i < cedr_config.getTotalResources(); i++) {
    if(hardware_thread_handle[i].is_sleeping == true)
    {
      pthread_cond_signal(&(hardware_thread_handle[i].resource_cond));
      hardware_thread_handle[i].is_sleeping = false;
    }
    pthread_join(resource_handle[i], nullptr);
  }

  LOG_INFO << "Terminated threads";
  sleep_time.tv_sec = 0;
  sleep_time.tv_nsec = 10000000;
  nanosleep(&sleep_time, nullptr);  
  
  // Log tasks from the completed task queues and free them
  unsigned int total_resource_count = cedr_config.getTotalResources();
  for (unsigned int i = 0; i < total_resource_count; i++) {
    completed_task_queue_length = hardware_thread_handle[i].completed_task_dequeue.size();
    while (completed_task_queue_length != 0) {
      task_nodes *task = hardware_thread_handle[i].completed_task_dequeue.front();
      hardware_thread_handle[i].completed_task_dequeue.pop_front();

      struct struct_logging log_obj {};
      strcpy(log_obj.app_name, task->app_pnt->app_name);
      log_obj.task_id = task->task_id;
      log_obj.app_id = task->app_pnt->app_id;
      strcpy(log_obj.task_name, task->task_name.c_str());
      strcpy(log_obj.assign_resource_name, task->assigned_resource_name.c_str());
      log_obj.start = task->start;
      log_obj.end = task->end;
      log_obj.input_size = task->input_size;
      log_obj.is_fft = task->is_fft;

      stream_timing_log.push_back(log_obj);
      free(task);

      completed_task_queue_length--;
    }
  }

  // Log completed applications from completed_nonstreaming_app list and free them
  for (auto completed_app = completed_nonstreaming_apps.begin(), completed_app_end = completed_nonstreaming_apps.end(); completed_app!=completed_app_end;) {
    struct struct_applogging applog_obj {};
    auto erase_app = (*completed_app); completed_app++;
    strncpy(applog_obj.app_name, erase_app->app_name, 50);
    applog_obj.app_id = erase_app->app_id;
    applog_obj.arrival = erase_app->arrival_time;
    applog_obj.start = erase_app->start_time;
    applog_obj.end = erase_app->finish_time; 
    applog_obj.app_runtime = applog_obj.end - applog_obj.start;
    applog_obj.app_lifetime = applog_obj.end - applog_obj.arrival;
    app_log.push_back(applog_obj);

    completed_nonstreaming_apps.remove(erase_app);
    free(erase_app);
  }


  uint64_t earliestStart = std::numeric_limits<uint64_t>::max();
  uint64_t latestFinish = 0;

#if defined(ENABLE_IL_ORACLE_LOGGING)
  ////////////////////////////////////
  // IL-Sched
  ////////////////////////////////////
  // Write out all the tasks (features and labels) to file
  ////////////////////////////////////
  if (cedr_config.getScheduler().compare("EFT") == 0 || cedr_config.getScheduler().compare("ETF") == 0) {
    std::list<struct_ILOracle_task> task_list;
    struct_ILOracle_task task_info;
    task_list = cedr_config.getILOracleTaskList();
    // task_it   = task_list.begin();

    std::list<struct_ILOracle_task>::iterator task_it;
    std::list<struct_ILOracle_PE>::iterator pe_it;

    // Check if trace file creation results in errors
    FILE *il_oracle_fp;
    il_oracle_fp = fopen((log_path + "/il_oracle.log").c_str(), "w");

    if (il_oracle_fp == nullptr){
      LOG_ERROR << "Error outputting IL Oracle file!";
    } else {
      // Iterate over all tasks in Oracle
      for (task_it = task_list.begin(); task_it != task_list.end(); ++task_it) {

        // Iterate over all PEs for each task
        for (pe_it = (*task_it).PEInfo.begin(); pe_it != (*task_it).PEInfo.end(); ++pe_it) {
            fprintf(il_oracle_fp, "%d,%" PRId64 ",", (*pe_it).isSupported, (*pe_it).finish_time);
        }
        fprintf(il_oracle_fp, "%d\n", (*task_it).resource_index);
      }

      // Close file handles and change permissions
      fclose(il_oracle_fp);
      chmod((log_path + "/il_oracle.csv").c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }
  }
  ////////////////////////////////////  

#else
  {
    // Sort the stream timing log based on start time of tasks
    // This resolves edge cases where tasks are pushed into the stream_timing_log in the "wrong" order because of
    // the order in which CEDR checks for completed applications across all PEs   
    stream_timing_log.sort([](struct struct_logging first, struct struct_logging second) {
      return (first.start) < (second.start);
    });

    std::list<struct struct_logging>::iterator it;
    it = stream_timing_log.begin();
    FILE *trace_fp = fopen((log_path + "/timing_trace.log").c_str(), "w");
    if (trace_fp == nullptr) {
      LOG_ERROR << "Error opening output trace file!";
    } else {
      while (it != stream_timing_log.end()) {

        struct struct_logging task = *it;
        uint64_t s0, e0;  
        s0 = (task.start);
        e0 = (task.end);      

        if (e0 > latestFinish) {
          latestFinish = e0;
        }
        if (s0 < earliestStart) {
          earliestStart = s0;
        }

        fprintf(trace_fp,
          "app_id: %d, app_name: %s, task_id: %d, task_name: %s, "
          "resource_name: %s, ref_start_time: %" PRIu64 ", ref_stop_time: %" PRIu64 ", "
          "actual_exe_time: %" PRIu64 ", input_size: %zu, is_fft: %s\n",
          task.app_id, task.app_name, task.task_id, task.task_name, task.assign_resource_name, s0, e0, e0 - s0, task.input_size,
          task.is_fft ? "true" : "false");

        it = stream_timing_log.erase(it);
      }
    }
    fclose(trace_fp);
    chmod((log_path + "/timing_trace.log").c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
  }

  // Schedule log capturing
  {
    std::list<struct struct_schedlogging>::iterator it;
    it = sched_log.begin();

    FILE *schedtrace_fp;
    schedtrace_fp = fopen((log_path + "/schedule_trace.log").c_str(), "w");
    if (schedtrace_fp == nullptr){
      LOG_ERROR << "Error outputting schedule trace file!";
    }
    else{
      uint64_t total_sched_overhead = 0;
      unsigned int total_ready_tasks = 0;
      while (it != sched_log.end()){
        struct struct_schedlogging schedlog_element = *it;

        uint64_t s1 = schedlog_element.start;
        uint64_t e1 = schedlog_element.end;

        fprintf(schedtrace_fp, "ready_queue_size: %u, ref_start_time: %" PRIu64 ", ref_stop_time: %" PRIu64 ", sheduling_overhead: %" PRIu64 " ns\n",
                schedlog_element.ready_queue_size, s1, e1, e1-s1);
        total_sched_overhead += (e1-s1);
        total_ready_tasks += schedlog_element.ready_queue_size;
        it = sched_log.erase(it);
      }
      fprintf(schedtrace_fp, "total_ready_tasks: %u, total_scheduling_overhead: %" PRIu64 " ns",
              total_ready_tasks, total_sched_overhead);
      fclose(schedtrace_fp);
      chmod((log_path + "/schedule_trace.log").c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }
  }

  // App Runtime log capturing
  // TODO: Add sorting of application logs by earliest to latest time, similar to task logging
  {
    std::list<struct struct_applogging>::iterator it;
    it = app_log.begin();

    FILE *apptrace_fp;
    apptrace_fp = fopen((log_path + "/appruntime_trace.log").c_str(), "w");
    if (apptrace_fp == nullptr){
      LOG_ERROR << "Error outputting schedule trace file!";
    }
    else{
      while (it != app_log.end()){
        struct struct_applogging applog_element = *it;

        fprintf(apptrace_fp, "app_id: %d, app_name: %s, ref_arrival_time: %" PRIu64 ", ref_start_time: %" PRIu64 ", ref_end_time: %" PRIu64 ", app_runtime: %" PRIu64 ", app_lifetime: %" PRIu64 " \n",
                applog_element.app_id, applog_element.app_name, applog_element.arrival, applog_element.start, applog_element.end, applog_element.app_runtime, applog_element.app_lifetime);
        it = app_log.erase(it);
      }
      fclose(apptrace_fp);
      chmod((log_path + "/appruntime_trace.log").c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
    }
  }
#endif

  free(ipc_cmd.message);
  closeAPIImplementations();
  LOG_INFO << "Run logs are available in : " << log_path;
  LOG_INFO << "The makespan of that log is: " << latestFinish - earliestStart << " ns (" << (latestFinish - earliestStart) / 1000 << " us)";
}
