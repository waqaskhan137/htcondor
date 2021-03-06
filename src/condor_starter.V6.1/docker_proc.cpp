/***************************************************************
 *
 * Copyright (C) 1990-2014, Condor Team, Computer Sciences Department,
 * University of Wisconsin-Madison, WI.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you
 * may not use this file except in compliance with the License.  You may
 * obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 ***************************************************************/

#include "condor_common.h"
#include "condor_classad.h"
#include "condor_config.h"
#include "docker_proc.h"
#include "starter.h"
#include "docker-api.h"

extern CStarter *Starter;

//
// TODO: Allow the use of HTCondor file-transfer to provide the image.
// May require understanding "self-hosting".
//

//
// TODO: Leverage the procd to gather usage information; Docker uses
// the full container ID as (part of) the cgroup identifier(s).
//

DockerProc::DockerProc( ClassAd * jobAd ) : VanillaProc( jobAd ) { }

DockerProc::~DockerProc() { }

int DockerProc::StartJob() {
	std::string imageID;
	if( ! JobAd->LookupString( ATTR_DOCKER_IMAGE, imageID ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "%s not defined in job ad, unable to start job.\n", ATTR_DOCKER_IMAGE );
		return FALSE;
	}

	std::string command;
	JobAd->LookupString( ATTR_JOB_CMD, command );
	dprintf( D_FULLDEBUG, "%s: '%s'\n", ATTR_JOB_CMD, command.c_str() );

	std::string sandboxPath = Starter->jic->jobRemoteIWD();

	//
	// This code is deliberately wrong, probably for backwards-compability.
	// (See the code in JICShadow::beginFileTransfer(), which assumes that
	// we transferred the executable if ATTR_TRANSFER_EXECUTABLE is unset.)
	// Rather than risk breaking anything by fixing condor_submit (which
	// does not set ATTR_TRANSFER_EXECUTABLE unless it's false) -- and
	// introducing a version dependency -- assume the executable was
	// transferred unless it was explicitly noted otherwise.
	//
	bool transferExecutable = true;
	JobAd->LookupBool( ATTR_TRANSFER_EXECUTABLE, transferExecutable );
	if( transferExecutable ) {
		command = sandboxPath + "/" + command;
	}

	ArgList args;
	args.SetArgV1SyntaxToCurrentPlatform();
	MyString argsError;
	if( ! args.AppendArgsFromClassAd( JobAd, & argsError ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to read job arguments from job ad: '%s'.\n", argsError.c_str() );
		return FALSE;
	}

	Env job_env;
	MyString env_errors;
	if( !Starter->GetJobEnv(JobAd,&job_env,&env_errors) ) {
		dprintf( D_ALWAYS, "Aborting DockerProc::StartJob: %s\n", env_errors.Value());
		return 0;
	}

	// The GlobalJobID is unsuitable by virtue its octothorpes.  This
	// construction is informative, but could be made even less likely
	// to collide if it had a timestamp.
	formatstr( containerName, "HTCJob%d_%d_%s_PID%d",
		Starter->jic->jobCluster(),
		Starter->jic->jobProc(),
		Starter->getMySlotName().c_str(), // note: this can be "" for single slot machines.
		getpid() );


	//
	// Do I/O redirection (includes streaming).
	//

	// getStdFile() returns -1 on error.
	int childFDs[3] = { -2, -2, -2 };

	if( -1 == (childFDs[0] = openStdFile( SFT_IN, NULL, true, "Input file" )) ) {
		dprintf( D_ALWAYS | D_FAILURE, "DockerProc::StartJob(): failed to open stdin.\n" );
		return FALSE;
	}
	if( -1 == (childFDs[1] = openStdFile( SFT_OUT, NULL, true, "Output file" )) ) {
		dprintf( D_ALWAYS | D_FAILURE, "DockerProc::StartJob(): failed to open stdout.\n" );
		daemonCore->Close_FD( childFDs[0] );
		return FALSE;
	}
	if( -1 == (childFDs[2] = openStdFile( SFT_ERR, NULL, true, "Error file" )) ) {
		dprintf( D_ALWAYS | D_FAILURE, "DockerProc::StartJob(): failed to open stderr.\n" );
		daemonCore->Close_FD( childFDs[0] );
		daemonCore->Close_FD( childFDs[1] );
		return FALSE;
	}


	CondorError err;
	// DockerAPI::run() returns a PID from daemonCore->Create_Process(), which
	// makes it suitable for passing up into VanillaProc.  This combination
	// will trigger the reaper(s) when the container terminates.
	int rv = DockerAPI::run( containerName, imageID, command, args, job_env, sandboxPath, JobPid, childFDs, err );
	if( rv < 0 ) {
		dprintf( D_ALWAYS | D_FAILURE, "DockerAPI::run( %s, %s, ... ) failed with return value %d\n", imageID.c_str(), command.c_str(), rv );
		return FALSE;
	}
	dprintf( D_FULLDEBUG, "DockerAPI::run() returned pid %d\n", JobPid );

	// TODO: Start a timer to poll for job usage updates.

	++num_pids; // Used by OsProc::PublishUpdateAd().
	return TRUE;
}


bool DockerProc::JobReaper( int pid, int status ) {
	dprintf( D_ALWAYS, "DockerProc::JobReaper()\n" );

	//
	// This should mean that the container has terminated.
	//
	if( pid == JobPid ) {
		//
		// Even running Docker in attached mode, we have a race condition
		// where this inspect (or rm) will report that the container is
		// still running.  I'm guessing that the attached docker process
		// is exiting when the container exits, not when the docker daemon
		// notices that the container has exited.
		//

		int rv = -1;
		bool running = false;
		ClassAd dockerAd;
		CondorError error;
		// Years of careful research.
		for( int i = 0; i < 20; ++i ) {
			rv = DockerAPI::inspect( containerName, & dockerAd, error );
			if( rv < 0 ) {
				dprintf( D_FULLDEBUG, "Failed to inspect (for removal) container '%s'; sleeping a second (%d already slept) to give Docker a chance to catch up.\n", containerName.c_str(), i );
				sleep( 1 );
				continue;
			}

			if( ! dockerAd.LookupBool( "Running", running ) ) {
				dprintf( D_FULLDEBUG, "Inspection of container '%s' failed to reveal its running state; sleeping a second (%d already slept) to give Docke a chance to catch up.\n", containerName.c_str(), i );
				sleep( 1 );
				continue;
			}

			if( running ) {
				dprintf( D_FULLDEBUG, "Inspection reveals that container '%s' is still running; sleeping a second (%d already slept) to give Docker a chance to catch up.\n", containerName.c_str(), i );
				sleep( 1 );
				continue;
			}

			break;
		}

// FIXME: Move all this shared conditional-checking into a function.

		if( rv < 0 ) {
			dprintf( D_ALWAYS | D_FAILURE, "Failed to inspect (for removal) container '%s'.\n", containerName.c_str() );
			return VanillaProc::JobReaper( pid, status );
		}

		if( ! dockerAd.LookupBool( "Running", running ) ) {
			dprintf( D_ALWAYS | D_FAILURE, "Inspection of container '%s' failed to reveal its running state.\n", containerName.c_str() );
			return VanillaProc::JobReaper( pid, status );
		}

		if( running ) {
			dprintf( D_ALWAYS | D_FAILURE, "Inspection reveals that container '%s' is still running.\n", containerName.c_str() );
			return VanillaProc::JobReaper( pid, status );
		}

		// FIXME: Rethink returning a classad.  Having to check for missing
		// attributes blows.

		// TODO: Set status appropriately (as if it were from waitpid()).
		int dockerStatus;
		if( ! dockerAd.LookupInteger( "ExitCode", dockerStatus ) ) {
			dprintf( D_ALWAYS | D_FAILURE, "Inspection of container '%s' failed to reveal its exit code.\n", containerName.c_str() );
			return VanillaProc::JobReaper( pid, status );
		}
		dprintf( D_FULLDEBUG, "Setting status of Docker job to %d.\n", dockerStatus );
		status = dockerStatus;

		// TODO: Record final job usage.

		// We don't have to do any process clean-up, because container.
		// We'll do the disk clean-up after we've transferred files.
	}

	// This helps to make ssh-to-job more plausible.
	return VanillaProc::JobReaper( pid, status );
}


//
// JobExit() is called after file transfer.
//
bool DockerProc::JobExit() {
	dprintf( D_ALWAYS, "DockerProc::JobExit()\n" );

	ClassAd dockerAd;
	CondorError error;
	int rv = DockerAPI::inspect( containerName, & dockerAd, error );
	if( rv < 0 ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to inspect (for removal) container '%s'.\n", containerName.c_str() );
		return VanillaProc::JobExit();
	}

	bool running;
	if( ! dockerAd.LookupBool( "Running", running ) ) {
		dprintf( D_ALWAYS | D_FAILURE, "Inspection of container '%s' failed to reveal its running state.\n", containerName.c_str() );
		return VanillaProc::JobExit();
	}
	if( running ) {
		dprintf( D_ALWAYS | D_FAILURE, "Inspection reveals that container '%s' is still running.\n", containerName.c_str() );
		return VanillaProc::JobExit();
	}

	rv = DockerAPI::rm( containerName, error );
	if( rv < 0 ) {
		dprintf( D_ALWAYS | D_FAILURE, "Failed to remove container '%s'.\n", containerName.c_str() );
	}

	return VanillaProc::JobExit();
}

void DockerProc::Suspend() {
	dprintf( D_ALWAYS, "DockerProc::Suspend()\n" );

	// TODO: docker pause ${containerName} only exists in Docker 1.1+.

	is_suspended = true;
}


void DockerProc::Continue() {
	dprintf( D_ALWAYS, "DockerProc::Continue()\n" );

	if( is_suspended ) {
		// TODO: docker unpause ${containerName} only exists in Docker 1.1+.

		is_suspended = false;
	}
}

//
// Setting requested_exit allows OsProc::JobExit() to handle telling the
// user why the job exited.
//

bool DockerProc::Remove() {
	dprintf( D_ALWAYS, "DockerProc::Remove()\n" );

	if( is_suspended ) { Continue(); }
	requested_exit = true;

	// TODO: docker kill --signal=${rm_kill_sig} ${containerName}

	// Do NOT send any signals to the waiting process.  It should only
	// react when the container does.

	// If rm_kill_sig is not SIGKILL, the process may linger.  Returning
	// false indicates that shutdown is pending.
	return false;
}


bool DockerProc::Hold() {
	dprintf( D_ALWAYS, "DockerProc::Hold()\n" );

	if( is_suspended ) { Continue(); }
	requested_exit = true;

	// TODO: docker kill --signal=${hold_kill_sig} ${containerName}

	// Do NOT send any signals to the waiting process.  It should only
	// react when the container does.

	// If rm_kill_sig is not SIGKILL, the process may linger.  Returning
	// false indicates that shutdown is pending.
	return false;
}


bool DockerProc::ShutdownGraceful() {
	dprintf( D_ALWAYS, "DockerProc::ShutdownGraceful()\n" );

	if( containerName.empty() ) {
		// We haven't started a Docker yet, probably because we're still
		// doing file transfer.  Since we're all done, just return true;
		// the FileTransfer object will clean itself up.
		return true;
	}

	if( is_suspended ) { Continue(); }
	requested_exit = true;

	// TODO: rm_kill_sig defaults to soft_kill_sig
	// TODO: docker kill --signal=${rm_kill_sig} ${containerName}

	// Do NOT send any signals to the waiting process.  It should only
	// react when the container does.

	// If rm_kill_sig is not SIGKILL, the process may linger.  Returning
	// false indicates that shutdown is pending.
	return false;
}


bool DockerProc::ShutdownFast() {
	dprintf( D_ALWAYS, "DockerProc::ShutdownFast()\n" );

	if( containerName.empty() ) {
		// We haven't started a Docker yet, probably because we're still
		// doing file transfer.  Since we're all done, just return true;
		// the FileTransfer object will clean itself up.
		return true;
	}

	// There's no point unpausing the container (and possibly swapping
	// it all back in again) if we're just going to be sending it a SIGKILL,
	// so don't bother to Continue() the process if it's been suspended.
	requested_exit = true;

	// TODO: docker kill --signal=SIGKILL ${containerName}

	// Do NOT send any signals to the waiting process.  It should only
	// react when the container does.

	// Based on the other comments, you'd expect this to return true.
	// It could, but it's simpler to just to let the usual routines
	// handle the job clean-up than to duplicate them all here.
	return false;
}


bool DockerProc::PublishUpdateAd( ClassAd * ad ) {
	dprintf( D_ALWAYS, "DockerProc::PublishUpdateAd()\n" );

	// TODO: get usage from procd somehow.  Set num_pids, if we can.  See
	// VanillaProc::PublishUpdateAd() for the attributes to set.

	//
	// If we want to use the existing reporting code (probably a good
	// idea), we'll need to make sure that m_proc_exited, is_checkpointed,
	// is_suspended, num_pids, and dumped_core are set for OsProc; and that
	// job_start_time, job_exit_time, and exit_status are set.
	//
	// We set is_suspended and num_pids already, except for the TODO above.
	// DockerProc::JobReaper() already sets m_proc_exited, exit_code, and
	// dumped_core (indirectly, via OsProc::JobReaper()).
	//
	// We will need to set is_checkpointed appropriately when we support it.
	//
	// TODO: We could approximate job_start_time and job_exit_time internally,
	// or set them during our status polling.
	//

	return OsProc::PublishUpdateAd( ad );
}


// TODO: Implement.
void DockerProc::PublishToEnv( Env * /* env */ ) {
	dprintf( D_ALWAYS, "DockerProc::PublishToEnv()\n" );
	return;
}


bool DockerProc::Detect() {
	dprintf( D_ALWAYS, "DockerProc::Detect()\n" );

	//
	// To turn off Docker, unset DOCKER.  DockerAPI::detect() will fail
	// but not complain to the log (unless D_FULLDEBUG) if so.
	//

	CondorError err;
	bool hasDocker = DockerAPI::detect( err ) == 0;

	return hasDocker;
}

bool DockerProc::Version( std::string & version ) {
	dprintf( D_ALWAYS, "DockerProc::Version()\n" );

	//
	// To turn off Docker, unset DOCKER.  DockerAPI::version() will fail
	// but not complain to the log (unless D_FULLDEBUG) if so.
	//

	CondorError err;
	bool foundVersion = DockerAPI::version( version, err ) == 0;

	return foundVersion;
}
