      MODULE ocean_control_mod
!
!svn $Id$
!================================================== Hernan G. Arango ===
!  Copyright (c) 2002-2019 The ROMS/TOMS Group                         !
!    Licensed under a MIT/X style license                              !
!    See License_ROMS.txt                                              !
!=======================================================================
!                                                                      !
!  ROMS/TOMS Nonlinear Model Driver:                                   !
!                                                                      !
!  This driver executes ROMS/TOMS standard nonlinear model.  It        !
!  controls the initialization, time-stepping, and finalization        !
!  of the nonlinear model execution following ESMF conventions:        !
!                                                                      !
!     ROMS_initialize                                                  !
!     ROMS_run                                                         !
!     ROMS_finalize                                                    !
!                                                                      !
!=======================================================================
!
      implicit none

      PRIVATE
      PUBLIC  :: ROMS_initialize
      PUBLIC  :: ROMS_run
      PUBLIC  :: ROMS_finalize

      CONTAINS

      SUBROUTINE ROMS_initialize (first, mpiCOMM)
!
!=======================================================================
!                                                                      !
!  This routine allocates and initializes ROMS/TOMS state variables    !
!  and internal and external parameters.                               !
!                                                                      !
!=======================================================================
!
      USE mod_param
      USE mod_parallel
#ifdef VERIFICATION
      USE mod_fourdvar
#endif
      USE mod_iounits
      USE mod_scalars
!
#ifdef MCT_LIB
# ifdef ATM_COUPLING
      USE ocean_coupler_mod, ONLY : initialize_ocn2atm_coupling
# endif
# ifdef WAV_COUPLING
      USE ocean_coupler_mod, ONLY : initialize_ocn2wav_coupling
# endif
#endif
      USE strings_mod,       ONLY : FoundError
!
!  Imported variable declarations.
!
      logical, intent(inout) :: first

      integer, intent(in), optional :: mpiCOMM
!
!  Local variable declarations.
!
      logical :: allocate_vars = .TRUE.

#ifdef DISTRIBUTE
      integer :: MyError, MySize
#endif
      integer :: chunk_size, ng, thread
#ifdef _OPENMP
      integer :: my_threadnum
#endif

#ifdef DISTRIBUTE
!
!-----------------------------------------------------------------------
!  Set distribute-memory (MPI) world communictor.
!-----------------------------------------------------------------------
!
      IF (PRESENT(mpiCOMM)) THEN
        OCN_COMM_WORLD=mpiCOMM
      ELSE
        OCN_COMM_WORLD=MPI_COMM_WORLD
      END IF
      CALL mpi_comm_rank (OCN_COMM_WORLD, MyRank, MyError)
      CALL mpi_comm_size (OCN_COMM_WORLD, MySize, MyError)
#endif
!
!-----------------------------------------------------------------------
!  On first pass, initialize model parameters a variables for all
!  nested/composed grids.  Notice that the logical switch "first"
!  is used to allow multiple calls to this routine during ensemble
!  configurations.
!-----------------------------------------------------------------------
!
      IF (first) THEN
        first=.FALSE.
!
!  Initialize parallel control switches. These scalars switches are
!  independent from standard input parameters.
!
        CALL initialize_parallel
!
!  Read in model tunable parameters from standard input. Allocate and
!  initialize variables in several modules after the number of nested
!  grids and dimension parameters are known.
!
        CALL inp_par (iNLM)
        IF (FoundError(exit_flag, NoError, __LINE__,                    &
     &                 __FILE__)) RETURN
#ifdef NEMURO_SAN
        CALL ini_fish (iNLM)
        IF (FoundError(exit_flag, NoError, __LINE__,                    &
     &                 __FILE__)) RETURN
# ifdef PREDATOR
        CALL ini_pred (iNLM)
        IF (FoundError(exit_flag, NoError, __LINE__,                    &
     &                 __FILE__)) RETURN
# endif
# ifdef FISHING_FLEET
        CALL ini_fleet (iNLM)
        IF (FoundError(exit_flag, NoError, __LINE__,                    &
     &                 __FILE__)) RETURN
# endif
#endif
!
!  Set domain decomposition tile partition range.  This range is
!  computed only once since the "first_tile" and "last_tile" values
!  are private for each parallel thread/node.
!
!$OMP PARALLEL
#if defined _OPENMP
      MyThread=my_threadnum()
#elif defined DISTRIBUTE
      MyThread=MyRank
#else
      MyThread=0
#endif
      DO ng=1,Ngrids
        chunk_size=(NtileX(ng)*NtileE(ng)+numthreads-1)/numthreads
        first_tile(ng)=MyThread*chunk_size
        last_tile (ng)=first_tile(ng)+chunk_size-1
      END DO
!$OMP END PARALLEL
!
!  Initialize internal wall clocks. Notice that the timings does not
!  includes processing standard input because several parameters are
!  needed to allocate clock variables.
!
        IF (Master) THEN
          WRITE (stdout,10)
 10       FORMAT (/,' Process Information:',/)
        END IF
!
        DO ng=1,Ngrids
!$OMP PARALLEL
          DO thread=THREAD_RANGE
            CALL wclock_on (ng, iNLM, 0, __LINE__, __FILE__)
          END DO
!$OMP END PARALLEL
        END DO
!
!  Allocate and initialize all model state arrays.
!
!$OMP PARALLEL
        CALL mod_arrays (allocate_vars)
!$OMP END PARALLEL

#ifdef VERIFICATION
!
!  Allocate and initialize observation arrays.
!
        CALL initialize_fourdvar
#endif
      END IF

#if defined MCT_LIB && (defined ATM_COUPLING || defined WAV_COUPLING)
!
!-----------------------------------------------------------------------
!  Initialize coupling streams between model(s).
!-----------------------------------------------------------------------
!
      DO ng=1,Ngrids
# ifdef ATM_COUPLING
        CALL initialize_ocn2atm_coupling (ng, MyRank)
# endif
# ifdef WAV_COUPLING
        CALL initialize_ocn2wav_coupling (ng, MyRank)
# endif
      END DO
#endif
!
!-----------------------------------------------------------------------
!  Initialize nonlinear model state variables over all nested grids,
!  if applicable.
!-----------------------------------------------------------------------
!
!$OMP PARALLEL
      CALL initial
!$OMP END PARALLEL
      IF (FoundError(exit_flag, NoError, __LINE__,                      &
     &               __FILE__)) RETURN
!
!  Initialize run or ensemble counter.
!
      Nrun=1

#ifdef VERIFICATION
!
!  Create NetCDF file for model solution at observation locations.
!
      IF (Nrun.eq.1) THEN
        DO ng=1,Ngrids
          LdefMOD(ng)=.TRUE.
          wrtNLmod(ng)=.TRUE.
          wrtObsScale(ng)=.TRUE.
          CALL def_mod (ng)
          IF (FoundError(exit_flag, NoError, __LINE__,                  &
     &                   __FILE__)) RETURN
        END DO
      END IF
#endif
#ifdef ENKF_RESTART
!
!  Create Ensenble Kalman Filter (EnKF) reastart NetCDF file.
!
      IF (Nrun.eq.1) THEN
        DO ng=1,Ngrids
          LdefDAI(ng)=.TRUE.
          CALL def_dai (ng)
          IF (FoundError(exit_flag, NoError, __LINE__,                  &
     &                   __FILE__)) RETURN
        END DO
      END IF
#endif

      RETURN
      END SUBROUTINE ROMS_initialize

      SUBROUTINE ROMS_run (RunInterval)
!
!=======================================================================
!                                                                      !
!  This routine runs ROMS/TOMS nonlinear model for the specified time  !
!  interval (seconds), RunInterval.  It RunInterval=0, ROMS advances   !
!  one single time-step.                                               !
!                                                                      !
!=======================================================================
!
      USE mod_param
      USE mod_parallel
#ifdef VERIFICATION
      USE mod_fourdvar
#endif
      USE mod_iounits
      USE mod_scalars
!
      USE strings_mod, ONLY : FoundError
!
!  Imported variable declarations.
!
      real(r8), intent(in) :: RunInterval            ! seconds
!
!  Local variable declarations.
!
#if defined MODEL_COUPLING && !defined MCT_LIB
      logical, save :: FirstPass = .TRUE.
#endif
      integer :: ng
#if defined MODEL_COUPLING && !defined MCT_LIB
      integer :: NstrStep, NendStep
#endif
      real (r8) :: MyRunInterval
!
!-----------------------------------------------------------------------
!  Time-step nonlinear model over all nested grids, if applicable.
#if defined MODEL_COUPLING && !defined MCT_LIB
!  On first pass, add a timestep to the coupling interval to account
!  for ROMS kernel delayed delayed output until next timestep.
#endif
!-----------------------------------------------------------------------
!
      MyRunInterval=RunInterval
      IF (Master) WRITE (stdout,'(1x)')
      DO ng=1,Ngrids
#if defined MODEL_COUPLING && !defined MCT_LIB
        step_counter(ng)=0
        NstrStep=iic(ng)
        IF (FirstPass) THEN
          NendStep=NstrStep+INT((RunInterval+dt(ng))/dt(ng))
          IF (ng.eq.1) MyRunInterval=MyRunInterval+dt(ng)
          FirstPass=.FALSE.
        ELSE
          NendStep=NstrStep+INT(MyRunInterval/dt(ng))
        END IF
        IF (Master) WRITE (stdout,10) 'NL', ng, NstrStep, NendStep
#else
        IF (Master) WRITE (stdout,10) 'NL', ng, ntstart(ng), ntend(ng)
#endif
      END DO
      IF (Master) WRITE (stdout,'(1x)')
!
!$OMP PARALLEL
#ifdef SOLVE3D
# if defined OFFLINE_BIOLOGY || defined OFFLINE_FLOATS
      CALL main3d_offline (MyRunInterval)
# else
      CALL main3d (MyRunInterval)
# endif
#else
      CALL main2d (MyRunInterval)
#endif
!$OMP END PARALLEL

      IF (FoundError(exit_flag, NoError, __LINE__,                      &
     &               __FILE__)) RETURN
!
 10   FORMAT (1x,a,1x,'ROMS/TOMS: started time-stepping:',              &
     &        ' (Grid: ',i2.2,' TimeSteps: ',i12.12,' - ',i12.12,')')

      RETURN
      END SUBROUTINE ROMS_run

      SUBROUTINE ROMS_finalize
!
!=======================================================================
!                                                                      !
!  This routine terminates ROMS/TOMS nonlinear model execution.        !
!                                                                      !
!=======================================================================
!
      USE mod_param
      USE mod_parallel
      USE mod_iounits
      USE mod_ncparam
      USE mod_scalars
#ifdef CICE_MODEL
      USE CICE_FinalMod
#endif
!
!  Local variable declarations.
!
      integer :: Fcount, ng, thread
#ifdef ENKF_RESTART
      integer :: tile
#endif

#ifdef ENKF_RESTART
!
!-----------------------------------------------------------------------
!  Write out initial conditions for the next time window of the Ensemble
!  Kalman (EnKF) filter.
!-----------------------------------------------------------------------
!
# ifdef DISTRIBUTE
      tile=MyRank
# else
      tile=-1
# endif
!
      IF (exit_flag.eq.NoError) THEN
        DO ng=1,Ngrids
          CALL wrt_dai (ng, tile)
        END DO
      END IF
#endif
#ifdef VERIFICATION
!
!-----------------------------------------------------------------------
!  Compute and report model-observation comparison statistics.
!-----------------------------------------------------------------------
!
      IF (exit_flag.eq.NoError) THEN
        DO ng=1,Ngrids
          CALL stats_modobs (ng)
        END DO
      END IF
#endif
!
!-----------------------------------------------------------------------
!  If blowing-up, save latest model state into RESTART NetCDF file.
!-----------------------------------------------------------------------
!
!  If cycling restart records, write solution into the next record.
!
      IF (exit_flag==1 .or. exit_flag==9) THEN
        DO ng=1,Ngrids
          IF (LwrtRST(ng)) THEN
            IF (Master) WRITE (stdout,10)
 10         FORMAT (/,' Blowing-up: Saving latest model state into ',   &
     &                ' RESTART file',/)
            Fcount=RST(ng)%Fcount
            IF (LcycleRST(ng).and.(RST(ng)%Nrec(Fcount).ge.2)) THEN
              RST(ng)%Rindex=2
              LcycleRST(ng)=.FALSE.
            END IF
            blowup=exit_flag
            exit_flag=NoError
            CALL wrt_rst (ng)
          END IF
        END DO
      END IF
!
!-----------------------------------------------------------------------
!  Stop model and time profiling clocks.  Close output NetCDF files.
!-----------------------------------------------------------------------
!
!  Stop time clocks.
!
      IF (Master) THEN
        WRITE (stdout,20)
 20     FORMAT (/,' Elapsed CPU time (seconds):',/)
      END IF
!
      DO ng=1,Ngrids
!$OMP PARALLEL
        DO thread=THREAD_RANGE
          CALL wclock_off (ng, iNLM, 0, __LINE__, __FILE__)
        END DO
!$OMP END PARALLEL
      END DO
!
!  Close IO files.
!
      CALL close_out

#ifdef CICE_MODEL
      CALL CICE_Finalize
#endif

      RETURN
      END SUBROUTINE ROMS_finalize

      END MODULE ocean_control_mod
