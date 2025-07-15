#include "scheduler.hpp"
#include "il_oracle.hpp"
#include <algorithm>
#include <climits>
#include <map>
#include <random>

std::map<std::string, int> schedule_cache;
std::unordered_map<unsigned int, unsigned int> resource_to_num_of_tasks_scheduled_map; // used by scheduleLUT

// NN_DATA_ML_SCHED nn_data;

void cacheScheduleDecision(task_nodes *task, int resource_id) {
  auto key = std::string(task->app_pnt->app_name) + "+" + std::string(task->task_name);
  LOG_VERBOSE << "Caching scheduling decision of key " << key << " with resource_id " << resource_id;
  schedule_cache[key] = resource_id;
}

bool attemptToAssignTaskToPE(ConfigManager &cedr_config, task_nodes *task, worker_thread *thread_handle, int idx) {
  LOG_VERBOSE << "Attempting to assign task " << std::string(task->task_name) << " to resource " << std::string(thread_handle->resource_name);
  if (!task->supported_resources[(uint8_t) thread_handle->thread_resource_type]) {
    LOG_INFO << "Scheduler attempted to assign task " << std::string(task->task_name) << " to a resource of type " << resource_type_names[(uint8_t) thread_handle->thread_resource_type]
              << ", but that resource is not supported by this task";
    //LOG_INFO << "Thread resource type of " << thread_handle->resource_name << " is incompatible with supported resources of " << std::string(task->app_pnt->app_name);
    return false;
  } else {
    if (!cedr_config.getEnableQueueing()) { // Non-queued mode
      // std::cout << "[scheduler.cpp] Entered the Non-queued conditional segment" << std::endl;
      pthread_mutex_lock(&(thread_handle->resource_mutex));
      const auto resource_state = thread_handle->resource_state;
      const auto todo_queue_size = thread_handle->todo_task_dequeue.size();
      const auto comp_queue_size = thread_handle->completed_task_dequeue.size();
      pthread_mutex_unlock(&(thread_handle->resource_mutex));
      if ((resource_state != 0) || (todo_queue_size > 0) || (comp_queue_size > 0)) { // Thread not idle or has a job lined up
        LOG_VERBOSE << "Resource state is " << resource_state << ", todo queue size is " << todo_queue_size << ", completed queue size is " << comp_queue_size;
        return false;
      }
    }
  }

  task->assigned_resource_type = thread_handle->thread_resource_type;
  task->assigned_resource_name = thread_handle->resource_name;
  task->actual_run_func = task->run_funcs[(uint8_t) thread_handle->thread_resource_type];

  if(cedr_config.getScheduler() == "EFT" || cedr_config.getScheduler() == "ETF" || cedr_config.getScheduler() == "HEFT_RT"){
    uint64_t curr_time = cedrGetTime(thread_handle->time_per_cycle);
    const uint64_t task_exec_ns = cedr_config.getDashExecTime(task->task_type, thread_handle->thread_resource_type);
    const uint64_t avail_time = thread_handle->thread_avail_time;
    thread_handle->thread_avail_time = (curr_time >= avail_time) ? curr_time + task_exec_ns : avail_time + task_exec_ns;
  }

  // Queuing vs. Non-queuing
  pthread_mutex_lock(&(thread_handle->resource_mutex));
  if(thread_handle->is_sleeping == true)
  {
    pthread_cond_signal(&(thread_handle->resource_cond));
    thread_handle->is_sleeping = false;
  }
  thread_handle->todo_task_dequeue.push_back(task);
  pthread_mutex_unlock(&(thread_handle->resource_mutex));

  LOG_INFO << "Task pushed to the to-do queue of " << std::string(thread_handle->resource_name);
  if (cedr_config.getCacheSchedules()) {
    cacheScheduleDecision(task, idx);
  }
  return true;
}

int scheduleCached(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle, bool *is_sleeping) {
  LOG_VERBOSE << "Schedule tasks based on previously cached schedules";
  unsigned int tasks_scheduled = 0;
  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
    bool task_allocated = false;
    const auto key = std::string((*itr)->app_pnt->app_name) + "+" + std::string((*itr)->task_name);
    LOG_VERBOSE << "Checking if task with key " << key << " is cached";

    if (schedule_cache.find(key) != schedule_cache.end()) {
      LOG_VERBOSE << "Key found, performing resource assignment";
      const auto val = schedule_cache.at(key);
      // Note: we don't need to cache the schedule again, so we pass in "cache schedule == false" here
      task_allocated = attemptToAssignTaskToPE(cedr_config, (*itr), &hardware_thread_handle[val], val);
    } else {
      LOG_VERBOSE << "Key not found, task remaining unscheduled";
    }

    if (task_allocated) {
      tasks_scheduled++;
      itr = ready_queue.erase(itr);
    } else {
      ++itr;
    }
  }
  return tasks_scheduled;
}

int scheduleLUT(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle,
                   uint32_t &free_resource_count) {
  
  unsigned int tasks_scheduled = 0;
  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
    int type_of_resource = LUT_array[(uint8_t)((*itr)->task_type)];
    unsigned int num_of_resources_per_type = cedr_config.getResourceArray()[type_of_resource];
    if(num_of_resources_per_type == 0)
    {
      type_of_resource = (int)resource_type::cpu;
      num_of_resources_per_type = cedr_config.getResourceArray()[type_of_resource];
    }
    unsigned int num_of_tasks_scheduled = resource_to_num_of_tasks_scheduled_map[type_of_resource];
    unsigned int resource_id = num_of_tasks_scheduled % num_of_resources_per_type;    
    unsigned int global_idx = cedr_config.getResourceToGlobalID(type_of_resource, resource_id);  
    LOG_INFO << "Scheduler attempted to assign task " << std::string((*itr)->task_name) << " num_of_resources_per_type: " << num_of_resources_per_type << " type_of_resource: "<< type_of_resource <<" num_of_tasks_scheduled: " << num_of_tasks_scheduled;
    bool task_allocated = attemptToAssignTaskToPE(cedr_config, (*itr), &hardware_thread_handle[global_idx], global_idx);
    if (task_allocated) {
      itr = ready_queue.erase(itr);
      tasks_scheduled++;
      num_of_tasks_scheduled++;
      resource_to_num_of_tasks_scheduled_map[type_of_resource] = num_of_tasks_scheduled;
    }
  }
  return tasks_scheduled;
}

int scheduleSimple(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle,
                   uint32_t &free_resource_count) {
  // Note: static initialization only happens on the first call, so this isn't actually overwritten with each call
  static unsigned int rand_resource = 0;
  unsigned int tasks_scheduled = 0;
  int num_of_resources = (int)cedr_config.getTotalResources();

  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
    bool task_allocated;
    for (int i = 0; i < num_of_resources; i++) {
      // Just keep trying to assign this task until one works
      task_allocated = attemptToAssignTaskToPE(cedr_config, (*itr), &hardware_thread_handle[rand_resource], rand_resource);
      rand_resource = ++rand_resource % num_of_resources;
      if (task_allocated) {
        tasks_scheduled++;
        itr = ready_queue.erase(itr);
        if (!cedr_config.getEnableQueueing()) {
          free_resource_count--;
        }
        break;
      }
    }
    if (!cedr_config.getEnableQueueing() && free_resource_count == 0) {
      break;
    }
    if (!task_allocated) {
      itr++;
    }
  }
  return tasks_scheduled;
}


int scheduleRandom(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle, 
                   uint32_t &free_resource_count) {
  std::shuffle(ready_queue.begin(), ready_queue.end(), std::default_random_engine(cedr_config.getRandomSeed()));
  return scheduleSimple(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
}

int scheduleMET(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle, 
                uint32_t &free_resource_count) {
  unsigned int tasks_scheduled = 0;

  // We will store the last resource we scheduled to as an index and then start our "search" for a resource of that type
  // from here i.e.: if the first task picks CPU 0, and the next task wants a resource of type "CPU", it will pick CPU 1
  static unsigned int resource_idx = 0;

  unsigned int total_resources = cedr_config.getTotalResources();
  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
    bool task_allocated = false;
    // Identify the resource type that yields minimum execution time for this task
    std::pair<resource_type, long long> min_resource = {resource_type::cpu, std::numeric_limits<long long>::max()};

    //for (const auto resourceType : (*itr)->supported_resources) {
    for (int i = total_resources - 1; i >= 0; i--) {
      auto resource_type = hardware_thread_handle[i].thread_resource_type;
      const auto estimated_exec = cedr_config.getDashExecTime((*itr)->task_type, resource_type); // (*itr)->estimated_execution[resourceType];
      auto resourceIsSupported = ((*itr)->supported_resources[(uint8_t) resource_type]);
      if (estimated_exec < min_resource.second && resourceIsSupported) {
        min_resource = {resource_type, estimated_exec};
      }
    }

    // Now look for an instance of that resource type and assign to that if it's free
    for (int i = 0; i < total_resources; i++) {
      LOG_VERBOSE << "On the " << i << "-th iteration of looking for a resource in MET, looking at resource " << resource_idx;
      if (min_resource.first == hardware_thread_handle[resource_idx].thread_resource_type) {
        LOG_VERBOSE << "Resource " << resource_idx << " is of the right type, attempting to schedule to it";
        task_allocated = attemptToAssignTaskToPE(cedr_config, (*itr), &hardware_thread_handle[resource_idx], (int)resource_idx);
        if (task_allocated) {
          // Use this static int as our resource counter to encourage "load balancing" among the resources
          resource_idx = ++resource_idx % cedr_config.getTotalResources();
          break;
        }
      }
      // Use this static int as our resource counter to encourage "load balancing" among the resources
      resource_idx = ++resource_idx % cedr_config.getTotalResources();
    }
    if (task_allocated) {
      tasks_scheduled++;
      itr = ready_queue.erase(itr);
      if (!cedr_config.getEnableQueueing()) {
        free_resource_count--;
        if (free_resource_count == 0)
          break;
      }
    } else {
      ++itr;
      LOG_DEBUG << "In MET scheduler, after checking resource type of all the resources, the task " << (*itr)->task_name << " was not assigned to anything.";
      LOG_DEBUG << "If there are no resources available that match the type that gives it minimum execution time, it "
                   "will never be scheduled!";
    }
  }

  return tasks_scheduled;
}

int scheduleHEFT_RT(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle, 
                    uint32_t &free_resource_count) {

  unsigned int tasks_scheduled = 0;

  //"Sort by Rank-U"
  std::sort(ready_queue.begin(), ready_queue.end(), [&cedr_config](task_nodes *first, task_nodes *second) {
    uint64_t first_avg_execution = 0, second_avg_execution = 0;
    uint64_t first_supported_resources = 0, second_supported_resources = 0;
    for (size_t idx = 0; idx < resource_type::NUM_RESOURCE_TYPES; idx++) {
      if (first->supported_resources[idx]) {
        first_avg_execution += cedr_config.getDashExecTime(first->task_type, (resource_type) idx);
        first_supported_resources++;
      }
    }
    first_avg_execution /= first_supported_resources;

    for (size_t idx = 0; idx < resource_type::NUM_RESOURCE_TYPES; idx++) {
      if (second->supported_resources[idx]) {
        second_avg_execution += cedr_config.getDashExecTime(second->task_type, (resource_type) idx);
        second_supported_resources++;
      }
    }
    second_avg_execution /= second_supported_resources;

    return first_avg_execution > second_avg_execution;
  });

  IF_LOG(plog::debug) {
    std::string logstr;
    logstr += "The sorted ready queue is given by\n[";
    for (const auto *task : ready_queue) {
      logstr += task->task_name + " ";
    }
    logstr += "]";
    LOG_DEBUG << logstr;
  }

  // Yes, time advances throughout this process, but calling "now" constant will probably lead to less erratic
  // scheduling behavior
  uint64_t curr_time = cedrGetTime(hardware_thread_handle[0].time_per_cycle);

  // Schedule in that sorted order, each task on its resource that minimizes EFT
  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
    bool task_allocated = false;

    task_nodes *this_task = (*itr);
    api_types this_api_type = this_task->task_type;
    LOG_DEBUG << "Attempting to schedule task " << this_task->task_name << " based on its earliest finish time";
    // Pair that stores the resource index in the hardware threads array along with the current minimum EFT value
    std::pair<int, uint64_t> selected_resource = {-1, std::numeric_limits<uint64_t>::max()};

    // Iterate over all the resources and pick the one with minimum EFT
    // TODO: Note, this currently assumes communication is 0 because we don't have a model for it in CEDR
    for (unsigned int i = 0; i < cedr_config.getTotalResources(); i++) {
      resource_type this_resource_type = hardware_thread_handle[i].thread_resource_type;
      // If we aren't supported on this resource type, skip it
      if (!this_task->supported_resources[this_resource_type]) {
        LOG_VERBOSE << "In HEFT_RT, task " << this_task->task_name << " does not support resource type " << resource_type_names[this_resource_type];
        continue;
      }

      uint64_t ready_time = hardware_thread_handle[i].thread_avail_time;
      uint64_t start_time = (curr_time >= ready_time) ? curr_time : ready_time;
      // TODO: Note, there's currently no concept of a PE queue having a "gap" for the insertion-based EFT to fill
      // TODO: As such, we currently just skip straight to saying "if we execute on this PE, it's going to be by adding
      // to the end"
      uint64_t eft = start_time + cedr_config.getDashExecTime(this_api_type, this_resource_type);
      LOG_VERBOSE << "For resource " << i << ", the calculated EFT was " << eft;
      if (eft < selected_resource.second) {
        LOG_VERBOSE << "This EFT value was smaller than " << selected_resource.second << ", so it is the new EFT";
        selected_resource = {i, eft};
      } else {
        LOG_VERBOSE << "This EFT value was not smaller than " << selected_resource.second << ", so the EFT is unchanged";
      }
    }

    LOG_DEBUG << "The earliest finish time for task " << this_task->task_name << " was given by (resource_id, eft): (" << selected_resource.first << ", "
              << selected_resource.second << ")";

    // Allocate to the resource with minimum EFT
    const auto idx = selected_resource.first;
    task_allocated = attemptToAssignTaskToPE(cedr_config, this_task, &hardware_thread_handle[idx], idx);

    if (task_allocated) {
      tasks_scheduled++;
      itr = ready_queue.erase(itr);
    } else {
      LOG_WARNING << "HEFT_RT failed to allocate a task to a PE despite choosing a PE it was compatible with. Moving on to another task and trying again later.";
      ++itr;
    }
  }

  return tasks_scheduled;
}

/*
int scheduleDNN(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle, 
                uint32_t &free_resource_count) {

  int pe_idx;
  unsigned int tasks_scheduled = 0;
  bool task_allocated = false;
  // LOG_DEBUG << "Ready_queue size at the beginning of scheduling is  " << ready_queue.size();
  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
    // LOG_DEBUG << "[SIMPLESCHEDULE] itr of Ready  " << std::string((*itr)->task_name);
    pe_idx = getPrediction(cedr_config, &nn_data, (*itr), hardware_thread_handle, 0);
    // printf("[DEBUG] PE=%d\n",pe_idx);
    // pe_idx = 0;
    // CHECK FOR ACCEL
    if (pe_idx < cedr_config.getTotalResources()) {
      task_allocated = attemptToAssignTaskToPE(cedr_config, (*itr), &hardware_thread_handle[pe_idx], pe_idx);
    } else {
      LOG_WARNING << "DNN selected PE with index " << pe_idx << " but that PE does not exist";
    }

    if (task_allocated) {
      tasks_scheduled++;
      itr = ready_queue.erase(itr);
    } else {
      itr++;
    }
  }

  return tasks_scheduled;
}

int scheduleRT(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle,
               uint32_t &free_resource_count) {

  int pe_idx;
  unsigned int tasks_scheduled = 0;
  bool task_allocated = false;
  // LOG_DEBUG << "Ready_queue size at the beginning of scheduling is  " << ready_queue.size();
  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
    // LOG_DEBUG << "[SIMPLESCHEDULE] itr of Ready  " << std::string((*itr)->task_name);
    pe_idx = getPrediction(cedr_config, &nn_data, (*itr), hardware_thread_handle, 1) % cedr_config.getTotalResources();

    if (pe_idx < cedr_config.getTotalResources()) {
      task_allocated = attemptToAssignTaskToPE(cedr_config, (*itr), &hardware_thread_handle[pe_idx], pe_idx);
    } else {
      LOG_WARNING << "DNN selected PE with index " << pe_idx << " but that PE does not exist";
    }

    if (task_allocated) {
      tasks_scheduled++;
      itr = ready_queue.erase(itr);
    } else {
      itr++;
    }
  }

  return tasks_scheduled;
}
*/

#if defined(ENABLE_IL_ORACLE_LOGGING)
int scheduleEFT(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle,
                uint32_t &free_resource_count) {

  unsigned int tasks_scheduled = 0;
  uint64_t current_time_ns = 0;  
  int eft_resource = 0;
  unsigned long long earliest_estimated_availtime = 0;
  bool task_allocated;

  int num_of_resources = (int)cedr_config.getTotalResources();
  // For loop to iterate over all tasks in Ready queue
  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {

    ////////////////////////////////////
    // IL-Sched
    ////////////////////////////////////
    struct_ILOracle_task task_info {};
    long long ref_time = 0;
    ////////////////////////////////////

    earliest_estimated_availtime = ULLONG_MAX;
    // For each task, iterate over all PE's to find the earliest finishing one
    for (int i = num_of_resources - 1; i >= 0; i--) {
      auto resourceType = hardware_thread_handle[i].thread_resource_type;
      auto resourceIsSupported = ((*itr)->supported_resources[(uint8_t) resourceType]);
      current_time_ns = cedrGetTime(hardware_thread_handle[i].time_per_cycle);
      auto avail_time = (hardware_thread_handle[i].thread_avail_time < current_time_ns) ? 0 : (hardware_thread_handle[i].thread_avail_time - current_time_ns);
      
      ////////////////////////////////////
      // IL-Sched
      ////////////////////////////////////
      // Get the PE related information, and add it to task structure
      struct_ILOracle_PE task_PE {};
      task_PE.isSupported = resourceIsSupported;
      task_PE.finish_time = avail_time;
      task_info.PEInfo.push_back(task_PE);
      //////////////////////////////////// 

      if(resourceIsSupported){
        auto finishTime = avail_time + cedr_config.getDashExecTime((*itr)->task_type, resourceType);
        if (finishTime < earliest_estimated_availtime) {
          earliest_estimated_availtime = finishTime;
          eft_resource = i;
        }
      }
    }

    ////////////////////////////////////
    // IL-Sched
    ////////////////////////////////////
    task_info.resource_index = eft_resource;
    cedr_config.pushILOracle(task_info);
    ////////////////////////////////////

    // Attempt to assign task on earliest finishing PE
    task_allocated = attemptToAssignTaskToPE(cedr_config, (*itr), &hardware_thread_handle[eft_resource], eft_resource);

    // If task allocated successfully
    //  1. Increment the number of scheduled tasks
    //  2. Remove the task from ready_queue
    // Else
    //  1. Go to next task in ready_queue
    if (task_allocated) {
      tasks_scheduled++;
      itr = ready_queue.erase(itr);
      if (!cedr_config.getEnableQueueing()) {
        free_resource_count--;
        if (free_resource_count == 0)
          break;
      }
    } else {
      itr++;
    }
  }
  return tasks_scheduled;
}

#else
int scheduleEFT(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle,
                uint32_t &free_resource_count) {

  unsigned int tasks_scheduled = 0;
  uint64_t current_time_ns = 0;  
  int eft_resource = 0;
  unsigned long long earliest_estimated_availtime = 0;
  bool task_allocated;

  current_time_ns = cedrGetTime(hardware_thread_handle[0].time_per_cycle);
  
  int num_of_resources = (int)cedr_config.getTotalResources();
  // For loop to iterate over all tasks in Ready queue
  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
    earliest_estimated_availtime = ULLONG_MAX;
    // For each task, iterate over all PE's to find the earliest finishing one
    for (int i = num_of_resources - 1; i >= 0; i--) {
      auto resourceType = hardware_thread_handle[i].thread_resource_type;
      auto resourceIsSupported = ((*itr)->supported_resources[(uint8_t) resourceType]);
      
      if(resourceIsSupported){

      
        auto avail_time = (hardware_thread_handle[i].thread_avail_time < current_time_ns) ? 0 : (hardware_thread_handle[i].thread_avail_time - current_time_ns);
        auto finishTime = avail_time + cedr_config.getDashExecTime((*itr)->task_type, resourceType); // (*itr)->estimated_execution[resourceType];
        
        if (finishTime < earliest_estimated_availtime) {
          earliest_estimated_availtime = finishTime;
          eft_resource = i;
        }
      }
    }

    // Attempt to assign task on earliest finishing PE
    task_allocated = attemptToAssignTaskToPE(cedr_config, (*itr), &hardware_thread_handle[eft_resource], eft_resource);

    // If task allocated successfully
    //  1. Increment the number of scheduled tasks
    //  2. Remove the task from ready_queue
    // Else
    //  1. Go to next task in ready_queue
    if (task_allocated) {
      tasks_scheduled++;
      itr = ready_queue.erase(itr);
      if (!cedr_config.getEnableQueueing()) {
        free_resource_count--;
        if (free_resource_count == 0)
          break;
      }
    } else {
      itr++;
    }
  }
  return tasks_scheduled;
}
#endif

int scheduleIL(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle,
                uint32_t &free_resource_count) {

  unsigned int tasks_scheduled = 0;
  struct timespec current_time {};
  int eft_resource = 0;
  unsigned long long earliest_estimated_availtime = 0;
  bool task_allocated;

  // For loop to iterate over all tasks in Ready queue
  for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {

    ////////////////////////////////////
    // IL-Sched
    ////////////////////////////////////
    struct_ILOracle_task task_info {};
    long long ref_time = 0;
    float *features = (float *) calloc(2 * (int)cedr_config.getTotalResources(), sizeof(float));
    ////////////////////////////////////

    // For each task, iterate over all PE's to find the earliest finishing one
    int counter = 0;
    for (int i = (int)cedr_config.getTotalResources() - 1; i >= 0; i--) {
      auto resourceType = hardware_thread_handle[i].thread_resource_type;

      uint64_t current_time_ns = cedrGetTime(hardware_thread_handle[i].time_per_cycle);
      auto avail_time = (hardware_thread_handle[i].thread_avail_time < current_time_ns) ? 0 : (hardware_thread_handle[i].thread_avail_time - current_time_ns);
      auto resourceIsSupported = ((*itr)->supported_resources[(uint8_t) resourceType]);

      ////////////////////////////////////
      // IL-Sched
      ////////////////////////////////////
      // Get the feature information and call the IL predict function
      features[(2*counter) + 0] = (float) resourceType;
      features[(2*counter) + 1] = (float) avail_time;
      counter += 1;
      ////////////////////////////////////
    }

    // Predict the execution resource using IL-DT policy
    eft_resource = predict(features);

    // Attempt to assign task on earliest finishing PE
    task_allocated = attemptToAssignTaskToPE(cedr_config, (*itr), &hardware_thread_handle[eft_resource], eft_resource);

    // If task allocated successfully
    //  1. Increment the number of scheduled tasks
    //  2. Remove the task from ready_queue
    // Else
    //  1. Go to next task in ready_queue
    if (task_allocated) {
      tasks_scheduled++;
      itr = ready_queue.erase(itr);
      if (!cedr_config.getEnableQueueing()) {
        free_resource_count--;
        if (free_resource_count == 0)
          break;
      }
    } else {
      itr++;
    }
  }
  return tasks_scheduled;
}

#if defined(ENABLE_IL_ORACLE_LOGGING)
int scheduleETF(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle,
                uint32_t &free_resource_count) {

  unsigned int tasks_scheduled = 0;
  struct timespec current_time {};
  int etf_resource = 0;
  unsigned long long earliest_estimated_availtime = 0;
  auto minTask = ready_queue.begin();
  int ready_queue_size = ready_queue.size();
  bool task_allocated;
  int num_of_resources = (int)cedr_config.getTotalResources();

  LOG_DEBUG << "scheduleETF:  num_of_resources" << num_of_resources << " \n ";

  struct timespec curr_timespec {};
  clock_gettime(CLOCK_MONOTONIC_RAW, &curr_timespec);
  long long curr_time = curr_timespec.tv_nsec + curr_timespec.tv_sec * SEC2NANOSEC;
  long long avail_time;
  long long task_exec_time;

  unsigned int total_resources = cedr_config.getTotalResources();

  for (int t = 0; t < ready_queue_size; t++) { // Should run a maximum iteration of size of the ready queue

    ////////////////////////////////////
    // IL-Sched
    ////////////////////////////////////

    struct_ILOracle_task min_task_info {};
    int min_etf_resource;
    ////////////////////////////////////

    earliest_estimated_availtime = ULLONG_MAX;
    // For loop for going over task list
    for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
      // Run EFT for each task -----------------------------------------

      ////////////////////////////////////
      // IL-Sched
      ////////////////////////////////////

      struct_ILOracle_task task_info {};
      bool current_task = false;

      uint64_t current_time_ns = cedrGetTime(hardware_thread_handle[0].time_per_cycle);  
      for (unsigned int i = 0; i < num_of_resources; i++) {
        auto resourceType = hardware_thread_handle[i].thread_resource_type;
        auto resourceIsSupported = ((*itr)->supported_resources[(uint8_t) resourceType]);

        uint64_t thread_avail_time = hardware_thread_handle[i].thread_avail_time;
        uint64_t avail_time = (thread_avail_time < current_time_ns) ? 0 : (thread_avail_time - current_time_ns);
        uint64_t finishTime = avail_time + cedr_config.getDashExecTime((*itr)->task_type, resourceType); // (*itr)->estimated_execution[resourceType];
        ////////////////////////////////////
        // IL-Sched
        ////////////////////////////////////
        // Get the PE related information, and add it to task structure
        struct_ILOracle_PE task_PE {};
        task_PE.isSupported = resourceIsSupported;
        task_PE.finish_time = avail_time; 
        task_info.PEInfo.push_back(task_PE);
        ////////////////////////////////////

        if (resourceIsSupported && finishTime < earliest_estimated_availtime) {
          earliest_estimated_availtime = finishTime;
          etf_resource = i;
          minTask = itr;
          current_task = true;
        }
        
        if (current_task) {
          ////////////////////////////////////
          // IL-Sched
          ////////////////////////////////////
          // Store the data pertaining to the min task
          min_task_info    = task_info;
          min_etf_resource = etf_resource;
          ////////////////////////////////////
        }               
      }

      // End of EFT ----------------------------------------------------
      // Go to next task
      itr++;
    }

    // assign minTask on desired EFT resource
    task_allocated = attemptToAssignTaskToPE(cedr_config, (*minTask), &hardware_thread_handle[etf_resource], etf_resource);

    if (task_allocated) {

      ////////////////////////////////////
      // IL-Sched
      ////////////////////////////////////
      min_task_info.resource_index = min_etf_resource;
      cedr_config.pushILOracle(min_task_info);
      ////////////////////////////////////

      tasks_scheduled++;
      minTask = ready_queue.erase(minTask);
      if (!cedr_config.getEnableQueueing()) {
        free_resource_count--;
        if (free_resource_count == 0)
          break;
      }
    } else {
      LOG_DEBUG << "ETF failed to schedule task, tasks remaining in ready_queue " << ready_queue.size() << " \n ";
    }
  }
  return tasks_scheduled;
}
#else
int scheduleETF(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle,
                uint32_t &free_resource_count) {

  unsigned int tasks_scheduled = 0;
  struct timespec current_time {};
  int etf_resource = 0;
  unsigned long long earliest_estimated_availtime = 0;
  auto minTask = ready_queue.begin();
  int ready_queue_size = ready_queue.size();
  bool task_allocated;
  int num_of_resources = (int)cedr_config.getTotalResources();

  uint64_t current_time_ns = cedrGetTime(hardware_thread_handle[0].time_per_cycle);   
  LOG_DEBUG << "scheduleETF:  num_of_resources" << num_of_resources << " \n ";


  for (int t = 0; t < ready_queue_size; t++) { // Should run a maximum iteration of size of the ready queue
    earliest_estimated_availtime = ULLONG_MAX;
    // For loop for going over task list
    for (auto itr = ready_queue.begin(); itr != ready_queue.end();) {
      // Run EFT for each task -----------------------------------------

      for (unsigned int i = 0; i < num_of_resources; i++) {
        auto resourceType = hardware_thread_handle[i].thread_resource_type;
        auto resourceIsSupported = ((*itr)->supported_resources[(uint8_t) resourceType]);
        
        if(resourceIsSupported){

            // printf("Resource %u (%s) avail time: %llu, current time: %llu, resource estimate: %llu\n", 
            //    i, hardware_thread_handle[i].resource_name, 
            //    hardware_thread_handle[i].thread_avail_time, current_time_ns, cedr_config.getDashExecTime((*itr)->task_type, resourceType));
          uint64_t thread_avail_time = hardware_thread_handle[i].thread_avail_time;
          uint64_t avail_time = (thread_avail_time < current_time_ns) ? 0 : (thread_avail_time - current_time_ns);
          uint64_t finishTime = avail_time + cedr_config.getDashExecTime((*itr)->task_type, resourceType);
          if (finishTime < earliest_estimated_availtime) {
            earliest_estimated_availtime = finishTime;
            etf_resource = i;
            minTask = itr;
          }                
        }      
      }

      // End of EFT ----------------------------------------------------
      // Go to next task
      itr++;
    }

    // assign minTask on desired EFT resource
    task_allocated = attemptToAssignTaskToPE(cedr_config, (*minTask), &hardware_thread_handle[etf_resource], etf_resource);

    if (task_allocated) {
      tasks_scheduled++;
      minTask = ready_queue.erase(minTask);
      if (!cedr_config.getEnableQueueing()) {
        free_resource_count--;
        if (free_resource_count == 0)
          break;
      }
    } else {
      LOG_DEBUG << "ETF failed to schedule task, tasks remaining in ready_queue " << ready_queue.size() << " \n ";
    }
  }
  return tasks_scheduled;
}
#endif

void performScheduling(ConfigManager &cedr_config, std::deque<task_nodes *> &ready_queue, worker_thread *hardware_thread_handle,
                       uint32_t &free_resource_count) {
  int tasks_scheduled = 0;
  size_t original_ready_queue_size = ready_queue.size();
  const std::string &sched_policy = cedr_config.getScheduler();

  if (original_ready_queue_size == 0) {
    printf("empty queue");
	return;
  }

  LOG_DEBUG << "Ready queue non-empty, performing task scheduling";
  //tasks_scheduled += scheduleSimple(cedr_config, ready_queue, hardware_thread_handle, resource_mutex, free_resource_count);
  // Begin by scheduling all cached tasks if requested
  /*if (cedr_config.getCacheSchedules()) {
    tasks_scheduled = scheduleCached(cedr_config, ready_queue, hardware_thread_handle, resource_mutex);
  }*/
  // Then schedule whatever is left with the user's chosen scheduler
  if (sched_policy == "SIMPLE") {
    tasks_scheduled += scheduleSimple(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  } else if (sched_policy == "LUT") {
    tasks_scheduled += scheduleLUT(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  } else if (sched_policy == "RANDOM") {
    tasks_scheduled += scheduleRandom(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  } else if (sched_policy == "MET") {
    tasks_scheduled += scheduleMET(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  } else if (sched_policy == "HEFT_RT") {
    tasks_scheduled += scheduleHEFT_RT(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  }/* else if (sched_policy == "DNN") {
    tasks_scheduled += scheduleDNN(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  } else if (sched_policy == "RT") {
    tasks_scheduled += scheduleRT(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  }*/ else if (sched_policy == "EFT") {
    tasks_scheduled += scheduleEFT(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  } else if (sched_policy == "IL") {
    tasks_scheduled += scheduleIL(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  }else if (sched_policy == "ETF") {
    tasks_scheduled += scheduleETF(cedr_config, ready_queue, hardware_thread_handle, free_resource_count);
  } else {
    LOG_FATAL << "Unknown scheduling policy selected! Exiting...";
    exit(1);
  }
  if (tasks_scheduled == 0 && original_ready_queue_size > 0) {
    LOG_WARNING << "During scheduling, no tasks were assigned despite the ready queue having " << original_ready_queue_size << " tasks";
	exit(1);
  } else {
    LOG_DEBUG << "Scheduled " << tasks_scheduled << " tasks. There are now " << free_resource_count << " free resources";
  }
}
