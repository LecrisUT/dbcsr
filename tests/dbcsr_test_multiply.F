!--------------------------------------------------------------------------------------------------!
! Copyright (C) by the DBCSR developers group - All rights reserved                                !
! This file is part of the DBCSR library.                                                          !
!                                                                                                  !
! For information on the license, see the LICENSE file.                                            !
! For further information please visit https://dbcsr.cp2k.org                                      !
! SPDX-License-Identifier: GPL-2.0+                                                                !
!--------------------------------------------------------------------------------------------------!

MODULE dbcsr_test_multiply
   !! Tests for DBCSR multiply
   USE dbcsr_data_methods, ONLY: dbcsr_data_get_sizes, &
                                 dbcsr_data_init, &
                                 dbcsr_data_new, &
                                 dbcsr_data_release, &
                                 dbcsr_scalar_negative, &
                                 dbcsr_scalar_one, &
                                 dbcsr_type_1d_to_2d
   USE dbcsr_dist_methods, ONLY: dbcsr_distribution_new, &
                                 dbcsr_distribution_release
   USE dbcsr_io, ONLY: dbcsr_print
   USE dbcsr_kinds, ONLY: real_4, &
                          real_8
   USE dbcsr_methods, ONLY: &
      dbcsr_col_block_offsets, dbcsr_col_block_sizes, dbcsr_get_data_type, &
      dbcsr_get_matrix_type, dbcsr_name, dbcsr_nblkcols_total, dbcsr_nblkrows_total, &
      dbcsr_nfullcols_total, dbcsr_nfullrows_total, dbcsr_release, dbcsr_row_block_offsets, &
      dbcsr_row_block_sizes
   USE dbcsr_mpiwrap, ONLY: mp_bcast, &
                            mp_environ, mp_comm_type
   USE dbcsr_multiply_api, ONLY: dbcsr_multiply
   USE dbcsr_operations, ONLY: dbcsr_copy, &
                               dbcsr_get_occupation, &
                               dbcsr_scale
   USE dbcsr_test_methods, ONLY: compx_to_dbcsr_scalar, &
                                 dbcsr_impose_sparsity, &
                                 dbcsr_make_random_block_sizes, &
                                 dbcsr_make_random_matrix, &
                                 dbcsr_random_dist, &
                                 dbcsr_to_dense_local
   USE dbcsr_transformations, ONLY: dbcsr_redistribute, &
                                    dbcsr_replicate_all
   USE dbcsr_types, ONLY: &
      dbcsr_conjugate_transpose, dbcsr_data_obj, dbcsr_distribution_obj, dbcsr_mp_obj, &
      dbcsr_no_transpose, dbcsr_scalar_type, dbcsr_transpose, dbcsr_type, &
      dbcsr_type_antisymmetric, dbcsr_type_complex_4, dbcsr_type_complex_4_2d, &
      dbcsr_type_complex_8, dbcsr_type_complex_8_2d, dbcsr_type_no_symmetry, dbcsr_type_real_4, &
      dbcsr_type_real_4_2d, dbcsr_type_real_8, dbcsr_type_real_8_2d, dbcsr_type_symmetric
   USE dbcsr_work_operations, ONLY: dbcsr_create
#include "base/dbcsr_base_uses.f90"

!$ USE OMP_LIB, ONLY: omp_get_max_threads, omp_get_thread_num, omp_get_num_threads

   IMPLICIT NONE

   PRIVATE

   PUBLIC :: dbcsr_test_multiplies

   CHARACTER(len=*), PARAMETER, PRIVATE :: moduleN = 'dbcsr_test_multiply'

   LOGICAL, PARAMETER :: debug_mod = .FALSE.

CONTAINS

   SUBROUTINE dbcsr_test_multiplies(test_name, mp_group, mp_env, npdims, io_unit, &
                                    matrix_sizes, bs_m, bs_n, bs_k, sparsities, &
                                    alpha, beta, limits, retain_sparsity)
      !! Performs a variety of matrix multiplies of same matrices on different
      !! processor grids

      CHARACTER(len=*), INTENT(IN)                       :: test_name
      TYPE(mp_comm_type), INTENT(IN)                                :: mp_group
         !! MPI communicator
      TYPE(dbcsr_mp_obj), INTENT(IN)                     :: mp_env
      INTEGER, DIMENSION(2), INTENT(in)                  :: npdims
      INTEGER, INTENT(IN)                                :: io_unit
         !! which unit to write to, if not negative
      INTEGER, DIMENSION(:), INTENT(in)                  :: matrix_sizes, bs_m, bs_n, bs_k
         !! size of matrices to test
         !! block sizes of the 3 dimension
         !! block sizes of the 3 dimension
         !! block sizes of the 3 dimension
      REAL(real_8), DIMENSION(3), INTENT(in)             :: sparsities
         !! sparsities of matrices to create
      COMPLEX(real_8), INTENT(in)                        :: alpha, beta
         !! alpha value to use in multiply
         !! beta value to use in multiply
      INTEGER, DIMENSION(6), INTENT(in)                  :: limits
      LOGICAL, INTENT(in)                                :: retain_sparsity

      CHARACTER(len=*), PARAMETER :: routineN = 'dbcsr_test_multiplies'
      CHARACTER, DIMENSION(3), PARAMETER :: &
         trans = (/dbcsr_no_transpose, dbcsr_transpose, dbcsr_conjugate_transpose/)
      CHARACTER, DIMENSION(3, 12), PARAMETER :: symmetries = &
                                                RESHAPE((/dbcsr_type_no_symmetry, dbcsr_type_no_symmetry, &
                                                          dbcsr_type_no_symmetry, dbcsr_type_symmetric, &
                                                          dbcsr_type_no_symmetry, dbcsr_type_no_symmetry, &
                                                          dbcsr_type_antisymmetric, dbcsr_type_no_symmetry, &
                                                          dbcsr_type_no_symmetry, dbcsr_type_no_symmetry, &
                                                          dbcsr_type_symmetric, dbcsr_type_no_symmetry, &
                                                          dbcsr_type_symmetric, dbcsr_type_symmetric, &
                                                          dbcsr_type_no_symmetry, dbcsr_type_antisymmetric, &
                                                          dbcsr_type_symmetric, dbcsr_type_no_symmetry, &
                                                          dbcsr_type_no_symmetry, dbcsr_type_antisymmetric, &
                                                          dbcsr_type_no_symmetry, dbcsr_type_symmetric, &
                                                          dbcsr_type_antisymmetric, dbcsr_type_no_symmetry, &
                                                          dbcsr_type_antisymmetric, dbcsr_type_antisymmetric, &
                                                          dbcsr_type_no_symmetry, dbcsr_type_no_symmetry, &
                                                          dbcsr_type_no_symmetry, dbcsr_type_symmetric, &
                                                          dbcsr_type_symmetric, dbcsr_type_symmetric, &
                                                          dbcsr_type_symmetric, dbcsr_type_antisymmetric, &
                                                          dbcsr_type_antisymmetric, dbcsr_type_symmetric/), (/3, 12/))
      INTEGER, DIMENSION(4), PARAMETER :: types = (/dbcsr_type_real_4, dbcsr_type_real_8, &
                                                    dbcsr_type_complex_4, dbcsr_type_complex_8/)

      CHARACTER                                          :: a_symm, b_symm, c_symm, transa, transb
      INTEGER                                            :: a_c, a_r, a_tr, b_c, b_r, b_tr, c_c, &
                                                            c_r, handle, isymm, itype, mynode, &
                                                            numnodes, numthreads, TYPE
      INTEGER, DIMENSION(:), POINTER, CONTIGUOUS         :: my_sizes_k, my_sizes_m, my_sizes_n, &
                                                            sizes_k, sizes_m, sizes_n
      LOGICAL                                            :: do_complex
      TYPE(dbcsr_data_obj)                               :: data_a, data_b, data_c, data_c_dbcsr
      TYPE(dbcsr_scalar_type)                            :: alpha_obj, beta_obj
      TYPE(dbcsr_type)                                   :: matrix_a, matrix_b, matrix_c

!   ---------------------------------------------------------------------------

      CALL timeset(routineN, handle)
      NULLIFY (my_sizes_k, my_sizes_m, my_sizes_n, &
               sizes_k, sizes_m, sizes_n)
      !
      ! print
      CALL mp_environ(numnodes, mynode, mp_group)
      IF (io_unit .GT. 0) THEN
         WRITE (io_unit, *) 'test_name ', test_name
         numthreads = 1
!$OMP PARALLEL
!$OMP MASTER
!$       numthreads = omp_get_num_threads()
!$OMP END MASTER
!$OMP END PARALLEL
         WRITE (io_unit, *) 'numthreads', numthreads
         WRITE (io_unit, *) 'numnodes', numnodes
         WRITE (io_unit, *) 'matrix_sizes', matrix_sizes
         WRITE (io_unit, *) 'sparsities', sparsities
         WRITE (io_unit, *) 'alpha', alpha
         WRITE (io_unit, *) 'beta', beta
         WRITE (io_unit, *) 'limits', limits
         WRITE (io_unit, *) 'retain_sparsity', retain_sparsity
         WRITE (io_unit, *) 'bs_m', bs_m
         WRITE (io_unit, *) 'bs_n', bs_n
         WRITE (io_unit, *) 'bs_k', bs_k
      END IF
      !
      !
      ! loop over symmetry
      DO isymm = 1, SIZE(symmetries, 2)
         a_symm = symmetries(1, isymm)
         b_symm = symmetries(2, isymm)
         c_symm = symmetries(3, isymm)

         IF (a_symm .NE. dbcsr_type_no_symmetry .AND. matrix_sizes(1) .NE. matrix_sizes(3)) CYCLE
         IF (b_symm .NE. dbcsr_type_no_symmetry .AND. matrix_sizes(2) .NE. matrix_sizes(3)) CYCLE
         IF (c_symm .NE. dbcsr_type_no_symmetry .AND. matrix_sizes(1) .NE. matrix_sizes(2)) CYCLE

         !
         ! loop over types
         DO itype = 1, SIZE(types)
            TYPE = types(itype)

            do_complex = TYPE .EQ. dbcsr_type_complex_4 .OR. TYPE .EQ. dbcsr_type_complex_8

            alpha_obj = compx_to_dbcsr_scalar(alpha, TYPE)
            beta_obj = compx_to_dbcsr_scalar(beta, TYPE)

            IF (do_complex .AND. c_symm == dbcsr_type_symmetric) CYCLE

            !
            ! loop over transpositions
            DO a_tr = 1, SIZE(trans)
            DO b_tr = 1, SIZE(trans)
               transa = trans(a_tr)
               transb = trans(b_tr)

               !
               ! if C has a symmetry, we need special transpositions
               IF (c_symm .NE. dbcsr_type_no_symmetry) THEN
                  IF (.NOT. (transa .EQ. dbcsr_no_transpose .AND. transb .EQ. dbcsr_transpose .OR. &
                             transa .EQ. dbcsr_transpose .AND. transb .EQ. dbcsr_no_transpose .OR. &
                             transa .EQ. dbcsr_no_transpose .AND. transb .EQ. dbcsr_conjugate_transpose .AND. &
                             .NOT. do_complex .OR. &
                             transa .EQ. dbcsr_conjugate_transpose .AND. transb .EQ. dbcsr_no_transpose .AND. &
                             .NOT. do_complex)) CYCLE
               END IF
               !
               ! if C has symmetry and special limits
               IF (c_symm .NE. dbcsr_type_no_symmetry) THEN
                  IF (limits(1) .NE. 1 .OR. limits(2) .NE. matrix_sizes(1) .OR. &
                      limits(3) .NE. 1 .OR. limits(4) .NE. matrix_sizes(2)) CYCLE
               END IF

               !
               ! Create the row/column block sizes.
               CALL dbcsr_make_random_block_sizes(sizes_m, matrix_sizes(1), bs_m)
               CALL dbcsr_make_random_block_sizes(sizes_n, matrix_sizes(2), bs_n)
               CALL dbcsr_make_random_block_sizes(sizes_k, matrix_sizes(3), bs_k)

               !
               ! if we have symmetry the row and column block sizes hae to match
               IF (c_symm .NE. dbcsr_type_no_symmetry .AND. a_symm .NE. dbcsr_type_no_symmetry .AND. &
                   b_symm .NE. dbcsr_type_no_symmetry) THEN
                  my_sizes_m => sizes_m
                  my_sizes_n => sizes_m
                  my_sizes_k => sizes_m
               ELSE IF ((c_symm .EQ. dbcsr_type_no_symmetry .AND. a_symm .NE. dbcsr_type_no_symmetry .AND. &
                         b_symm .NE. dbcsr_type_no_symmetry) .OR. &
                        (c_symm .NE. dbcsr_type_no_symmetry .AND. a_symm .EQ. dbcsr_type_no_symmetry .AND. &
                         b_symm .NE. dbcsr_type_no_symmetry) .OR. &
                        (c_symm .NE. dbcsr_type_no_symmetry .AND. a_symm .NE. dbcsr_type_no_symmetry .AND. &
                         b_symm .EQ. dbcsr_type_no_symmetry)) THEN
                  my_sizes_m => sizes_m
                  my_sizes_n => sizes_m
                  my_sizes_k => sizes_m
               ELSE IF (c_symm .EQ. dbcsr_type_no_symmetry .AND. a_symm .EQ. dbcsr_type_no_symmetry .AND. &
                        b_symm .NE. dbcsr_type_no_symmetry) THEN
                  my_sizes_m => sizes_m
                  my_sizes_n => sizes_n
                  my_sizes_k => sizes_n
               ELSE IF (c_symm .EQ. dbcsr_type_no_symmetry .AND. a_symm .NE. dbcsr_type_no_symmetry .AND. &
                        b_symm .EQ. dbcsr_type_no_symmetry) THEN
                  my_sizes_m => sizes_m
                  my_sizes_n => sizes_n
                  my_sizes_k => sizes_m
               ELSE IF (c_symm .NE. dbcsr_type_no_symmetry .AND. a_symm .EQ. dbcsr_type_no_symmetry .AND. &
                        b_symm .EQ. dbcsr_type_no_symmetry) THEN
                  my_sizes_m => sizes_m
                  my_sizes_n => sizes_m
                  my_sizes_k => sizes_k
               ELSE IF (c_symm .EQ. dbcsr_type_no_symmetry .AND. a_symm .EQ. dbcsr_type_no_symmetry .AND. &
                        b_symm .EQ. dbcsr_type_no_symmetry) THEN
                  my_sizes_m => sizes_m
                  my_sizes_n => sizes_n
                  my_sizes_k => sizes_k
               ELSE
                  CALL dbcsr_abort(__LOCATION__, &
                                   "something wrong here... ")
               END IF

               IF (.FALSE.) THEN
                  WRITE (*, *) 'sizes_m', my_sizes_m
                  WRITE (*, *) 'sum(sizes_m)', SUM(my_sizes_m), ' matrix_sizes(1)', matrix_sizes(1)
                  WRITE (*, *) 'sizes_n', my_sizes_n
                  WRITE (*, *) 'sum(sizes_n)', SUM(my_sizes_n), ' matrix_sizes(2)', matrix_sizes(2)
                  WRITE (*, *) 'sizes_k', my_sizes_k
                  WRITE (*, *) 'sum(sizes_k)', SUM(my_sizes_k), ' matrix_sizes(3)', matrix_sizes(3)
               END IF

               !
               ! Create the undistributed matrices.
               CALL dbcsr_make_random_matrix(matrix_c, my_sizes_m, my_sizes_n, "Matrix C", &
                                             sparsities(3), &
                                             mp_group, data_type=TYPE, symmetry=c_symm)

               IF (transa .NE. dbcsr_no_transpose) THEN
                  CALL dbcsr_make_random_matrix(matrix_a, my_sizes_k, my_sizes_m, "Matrix A", &
                                                sparsities(1), &
                                                mp_group, data_type=TYPE, symmetry=a_symm)
               ELSE
                  CALL dbcsr_make_random_matrix(matrix_a, my_sizes_m, my_sizes_k, "Matrix A", &
                                                sparsities(1), &
                                                mp_group, data_type=TYPE, symmetry=a_symm)
               END IF
               IF (transb .NE. dbcsr_no_transpose) THEN
                  CALL dbcsr_make_random_matrix(matrix_b, my_sizes_n, my_sizes_k, "Matrix B", &
                                                sparsities(2), &
                                                mp_group, data_type=TYPE, symmetry=b_symm)
               ELSE
                  CALL dbcsr_make_random_matrix(matrix_b, my_sizes_k, my_sizes_n, "Matrix B", &
                                                sparsities(2), &
                                                mp_group, data_type=TYPE, symmetry=b_symm)
               END IF

               DEALLOCATE (sizes_m, sizes_n, sizes_k)

               !
               ! if C has a symmetry, we build it accordingly, i.e. C=A*A and C=A*(-A)
               IF (c_symm .NE. dbcsr_type_no_symmetry) THEN
                  CALL dbcsr_copy(matrix_b, matrix_a)
                  !print*, a_symm,b_symm,dbcsr_get_matrix_type(matrix_a),dbcsr_get_matrix_type(matrix_b)
                  IF (c_symm .EQ. dbcsr_type_antisymmetric) THEN
                     CALL dbcsr_scale(matrix_b, &
                                      alpha_scalar=dbcsr_scalar_negative( &
                                      dbcsr_scalar_one(TYPE)))
                  END IF
               END IF

               !
               ! convert the dbcsr matrices to denses
               a_r = dbcsr_nfullrows_total(matrix_a)
               a_c = dbcsr_nfullcols_total(matrix_a)
               b_r = dbcsr_nfullrows_total(matrix_b)
               b_c = dbcsr_nfullcols_total(matrix_b)
               c_r = dbcsr_nfullrows_total(matrix_c)
               c_c = dbcsr_nfullcols_total(matrix_c)
               CALL dbcsr_data_init(data_a)
               CALL dbcsr_data_init(data_b)
               CALL dbcsr_data_init(data_c)
               CALL dbcsr_data_init(data_c_dbcsr)
               CALL dbcsr_data_new(data_a, dbcsr_type_1d_to_2d(TYPE), data_size=a_r, data_size2=a_c)
               CALL dbcsr_data_new(data_b, dbcsr_type_1d_to_2d(TYPE), data_size=b_r, data_size2=b_c)
               CALL dbcsr_data_new(data_c, dbcsr_type_1d_to_2d(TYPE), data_size=c_r, data_size2=c_c)
               CALL dbcsr_data_new(data_c_dbcsr, dbcsr_type_1d_to_2d(TYPE), data_size=c_r, data_size2=c_c)
               CALL dbcsr_to_dense_local(matrix_a, data_a)
               CALL dbcsr_to_dense_local(matrix_b, data_b)
               CALL dbcsr_to_dense_local(matrix_c, data_c)

               !
               ! Prepare test parameters
               CALL test_multiply(test_name, mp_group, mp_env, npdims, io_unit, &
                                  matrix_a, matrix_b, matrix_c, &
                                  data_a, data_b, data_c, data_c_dbcsr, &
                                  transa, transb, &
                                  alpha_obj, beta_obj, &
                                  limits, retain_sparsity)
               !
               ! cleanup
               CALL dbcsr_release(matrix_a)
               CALL dbcsr_release(matrix_b)
               CALL dbcsr_release(matrix_c)
               CALL dbcsr_data_release(data_a)
               CALL dbcsr_data_release(data_b)
               CALL dbcsr_data_release(data_c)
               CALL dbcsr_data_release(data_c_dbcsr)

            END DO
            END DO

         END DO ! itype

      END DO !isymm

      CALL timestop(handle)

   END SUBROUTINE dbcsr_test_multiplies

   SUBROUTINE test_multiply(test_name, mp_group, mp_env, npdims, io_unit, &
                            matrix_a, matrix_b, matrix_c, &
                            data_a, data_b, data_c, data_c_dbcsr, &
                            transa, transb, alpha, beta, limits, retain_sparsity)
      !! Performs a variety of matrix multiplies of same matrices on different
      !! processor grids

      CHARACTER(len=*), INTENT(IN)                       :: test_name
      TYPE(mp_comm_type), INTENT(IN)                                :: mp_group
         !! MPI communicator
      TYPE(dbcsr_mp_obj), INTENT(IN)                     :: mp_env
      INTEGER, DIMENSION(2), INTENT(in)                  :: npdims
      INTEGER, INTENT(IN)                                :: io_unit
         !! which unit to write to, if not negative
      TYPE(dbcsr_type), INTENT(in)                       :: matrix_a, matrix_b, matrix_c
         !! matrices to multiply
         !! matrices to multiply
         !! matrices to multiply
      TYPE(dbcsr_data_obj)                               :: data_a, data_b, data_c, data_c_dbcsr
      CHARACTER, INTENT(in)                              :: transa, transb
      TYPE(dbcsr_scalar_type), INTENT(in)                :: alpha, beta
      INTEGER, DIMENSION(6), INTENT(in)                  :: limits
      LOGICAL, INTENT(in)                                :: retain_sparsity

      CHARACTER(len=*), PARAMETER :: routineN = 'test_multiply'

      INTEGER                                            :: c_a, c_b, c_c, handle, r_a, r_b, r_c
      INTEGER, DIMENSION(:), POINTER, CONTIGUOUS         :: blk_offsets, col_dist_a, col_dist_b, &
                                                            col_dist_c, row_dist_a, row_dist_b, &
                                                            row_dist_c
      LOGICAL                                            :: success
      REAL(real_8)                                       :: occ_a, occ_b, occ_c_in, occ_c_out
      TYPE(dbcsr_distribution_obj)                       :: dist_a, dist_b, dist_c
      TYPE(dbcsr_type)                                   :: m_a, m_b, m_c

!   ---------------------------------------------------------------------------

      CALL timeset(routineN, handle)
      NULLIFY (row_dist_a, col_dist_a, &
               row_dist_b, col_dist_b, &
               row_dist_c, col_dist_c)

      IF (debug_mod .AND. io_unit .GT. 0) THEN
         WRITE (io_unit, *) REPEAT("*", 70)
         WRITE (io_unit, *) " -- TESTING dbcsr_multiply (", transa, ", ", transb, &
            ", ", dbcsr_get_data_type(m_a), &
            ", ", dbcsr_get_matrix_type(m_a), &
            ", ", dbcsr_get_matrix_type(m_b), &
            ", ", dbcsr_get_matrix_type(m_c), &
            ") ............... !"
         WRITE (io_unit, *) REPEAT("*", 70)
      END IF

      ! Row & column distributions
      CALL dbcsr_random_dist(row_dist_a, dbcsr_nblkrows_total(matrix_a), npdims(1))
      CALL dbcsr_random_dist(col_dist_a, dbcsr_nblkcols_total(matrix_a), npdims(2))
      CALL dbcsr_random_dist(row_dist_b, dbcsr_nblkrows_total(matrix_b), npdims(1))
      CALL dbcsr_random_dist(col_dist_b, dbcsr_nblkcols_total(matrix_b), npdims(2))
      CALL dbcsr_random_dist(row_dist_c, dbcsr_nblkrows_total(matrix_c), npdims(1))
      CALL dbcsr_random_dist(col_dist_c, dbcsr_nblkcols_total(matrix_c), npdims(2))
      CALL dbcsr_distribution_new(dist_a, mp_env, row_dist_a, col_dist_a, reuse_arrays=.TRUE.)
      CALL dbcsr_distribution_new(dist_b, mp_env, row_dist_b, col_dist_b, reuse_arrays=.TRUE.)
      CALL dbcsr_distribution_new(dist_c, mp_env, row_dist_c, col_dist_c, reuse_arrays=.TRUE.)
      ! Redistribute the matrices
      ! A
      CALL dbcsr_create(m_a, "Test for "//TRIM(dbcsr_name(matrix_a)), &
                        dist_a, dbcsr_get_matrix_type(matrix_a), &
                        row_blk_size_obj=matrix_a%row_blk_size, &
                        col_blk_size_obj=matrix_a%col_blk_size, &
                        data_type=dbcsr_get_data_type(matrix_a))
      CALL dbcsr_distribution_release(dist_a)
      CALL dbcsr_redistribute(matrix_a, m_a)
      ! B
      CALL dbcsr_create(m_b, "Test for "//TRIM(dbcsr_name(matrix_b)), &
                        dist_b, dbcsr_get_matrix_type(matrix_b), &
                        row_blk_size_obj=matrix_b%row_blk_size, &
                        col_blk_size_obj=matrix_b%col_blk_size, &
                        data_type=dbcsr_get_data_type(matrix_b))
      CALL dbcsr_distribution_release(dist_b)
      CALL dbcsr_redistribute(matrix_b, m_b)
      ! C
      CALL dbcsr_create(m_c, "Test for "//TRIM(dbcsr_name(matrix_c)), &
                        dist_c, dbcsr_get_matrix_type(matrix_c), &
                        row_blk_size_obj=matrix_c%row_blk_size, &
                        col_blk_size_obj=matrix_c%col_blk_size, &
                        data_type=dbcsr_get_data_type(matrix_c))
      CALL dbcsr_distribution_release(dist_c)
      CALL dbcsr_redistribute(matrix_c, m_c)

      IF (.FALSE.) THEN
         blk_offsets => dbcsr_row_block_offsets(matrix_c)
         WRITE (*, *) 'row_block_offsets(matrix_c)', blk_offsets
         blk_offsets => dbcsr_col_block_offsets(matrix_c)
         WRITE (*, *) 'col_block_offsets(matrix_c)', blk_offsets
      END IF

      IF (.FALSE.) THEN
         CALL dbcsr_print(m_c, matlab_format=.FALSE., variable_name='c_in_')
         CALL dbcsr_print(m_a, matlab_format=.FALSE., variable_name='a_')
         CALL dbcsr_print(m_b, matlab_format=.FALSE., variable_name='b_')
         CALL dbcsr_print(m_c, matlab_format=.FALSE., variable_name='c_out_')
      END IF

      occ_a = dbcsr_get_occupation(m_a)
      occ_b = dbcsr_get_occupation(m_b)
      occ_c_in = dbcsr_get_occupation(m_c)

      !
      ! Perform multiply
      IF (ALL(limits == 0)) THEN
         DBCSR_ABORT("limits shouldnt be 0")
      ELSE
         CALL dbcsr_multiply(transa, transb, alpha, &
                             m_a, m_b, beta, m_c, &
                             first_row=limits(1), &
                             last_row=limits(2), &
                             first_column=limits(3), &
                             last_column=limits(4), &
                             first_k=limits(5), &
                             last_k=limits(6), &
                             retain_sparsity=retain_sparsity)
      END IF

      occ_c_out = dbcsr_get_occupation(m_c)

      IF (.FALSE.) THEN
         PRINT *, 'retain_sparsity', retain_sparsity, occ_a, occ_b, occ_c_in, occ_c_out
         CALL dbcsr_print(m_a, matlab_format=.TRUE., variable_name='a_')
         CALL dbcsr_print(m_b, matlab_format=.TRUE., variable_name='b_')
         CALL dbcsr_print(m_c, matlab_format=.FALSE., variable_name='c_out_')
      END IF

      CALL dbcsr_replicate_all(m_c)
      CALL dbcsr_to_dense_local(m_c, data_c_dbcsr)
      CALL dbcsr_check_multiply(test_name, m_c, data_c_dbcsr, data_a, data_b, data_c, &
                                transa, transb, alpha, beta, limits, retain_sparsity, io_unit, mp_group, &
                                success)

      r_a = dbcsr_nfullrows_total(m_a)
      c_a = dbcsr_nfullcols_total(m_a)
      r_b = dbcsr_nfullrows_total(m_b)
      c_b = dbcsr_nfullcols_total(m_b)
      r_c = dbcsr_nfullrows_total(m_c)
      c_c = dbcsr_nfullcols_total(m_c)
      IF (io_unit .GT. 0) THEN
         IF (success) THEN
            WRITE (io_unit, *) REPEAT("*", 70)
            WRITE (io_unit, *) " -- TESTING dbcsr_multiply (", transa, ", ", transb, &
               ", ", dbcsr_get_data_type(m_a), &
               ", ", dbcsr_get_matrix_type(m_a), &
               ", ", dbcsr_get_matrix_type(m_b), &
               ", ", dbcsr_get_matrix_type(m_c), &
               ") ............... PASSED !"
            WRITE (io_unit, *) REPEAT("*", 70)
         ELSE
            WRITE (io_unit, *) REPEAT("*", 70)
            WRITE (io_unit, *) " -- TESTING dbcsr_multiply (", transa, ", ", transb, &
               ", ", dbcsr_get_data_type(m_a), &
               ", ", dbcsr_get_matrix_type(m_a), &
               ", ", dbcsr_get_matrix_type(m_b), &
               ", ", dbcsr_get_matrix_type(m_c), &
               ") ... FAILED !"
            WRITE (io_unit, *) REPEAT("*", 70)
            DBCSR_ABORT('Test failed')
         END IF
      END IF

      CALL dbcsr_release(m_a)
      CALL dbcsr_release(m_b)
      CALL dbcsr_release(m_c)

      CALL timestop(handle)

   END SUBROUTINE test_multiply

   SUBROUTINE dbcsr_check_multiply(test_name, matrix_c, dense_c_dbcsr, dense_a, dense_b, dense_c, &
                                   transa, transb, alpha, beta, limits, retain_sparsity, io_unit, mp_group, &
                                   success)
      !! Performs a check of matrix multiplies

      CHARACTER(len=*), INTENT(IN)                       :: test_name
      TYPE(dbcsr_type), INTENT(IN)                       :: matrix_c
      TYPE(dbcsr_data_obj), INTENT(inout)                :: dense_c_dbcsr, dense_a, dense_b, dense_c
         !! dense result of the dbcsr_multiply
         !! input dense matrices
         !! input dense matrices
         !! input dense matrices
      CHARACTER, INTENT(in)                              :: transa, transb
         !! transposition status
         !! transposition status
      TYPE(dbcsr_scalar_type), INTENT(in)                :: alpha, beta
         !! coefficients for the gemm
         !! coefficients for the gemm
      INTEGER, DIMENSION(6), INTENT(in)                  :: limits
         !! limits for the gemm
      LOGICAL, INTENT(in)                                :: retain_sparsity
      INTEGER, INTENT(IN)                                :: io_unit
         !! io unit for printing
      TYPE(mp_comm_type), INTENT(IN)                     :: mp_group
      LOGICAL, INTENT(out)                               :: success
         !! if passed the check success=T

      CHARACTER(len=*), PARAMETER :: routineN = 'dbcsr_check_multiply'
      INTEGER :: a_col, a_m, a_n, a_row, b_col, b_m, b_n, b_row, c_col, c_col_size, c_row, &
                 c_row_size, handle, i, istat, j, k, lda, ldb, ldc, lwork, m, mynode, n, numnodes
      CHARACTER, PARAMETER                               :: norm = 'I'

      LOGICAL                                            :: valid
      REAL(real_4), ALLOCATABLE, DIMENSION(:)            :: work_sp
#if defined (__ACCELERATE)
      REAL(real_8), EXTERNAL                             :: clange, slamch, slange
#else
      REAL(real_4), EXTERNAL                             :: clange, slamch, slange
#endif
      REAL(real_8)                                       :: a_norm, b_norm, c_norm_dbcsr, c_norm_in, &
                                                            c_norm_out, eps, eps_norm, residual
      REAL(real_8), ALLOCATABLE, DIMENSION(:)            :: work
      REAL(real_8), EXTERNAL                             :: dlamch, dlange, zlange

      CALL timeset(routineN, handle)

      CALL mp_environ(numnodes, mynode, mp_group)

      CALL dbcsr_data_get_sizes(dense_c, c_row_size, c_col_size, valid)
      IF (.NOT. valid) &
         DBCSR_ABORT("dense matrix not valid")
      CALL dbcsr_data_get_sizes(dense_c, ldc, i, valid)
      IF (.NOT. valid) &
         DBCSR_ABORT("dense matrix not valid")
      CALL dbcsr_data_get_sizes(dense_a, lda, i, valid)
      IF (.NOT. valid) &
         DBCSR_ABORT("dense matrix not valid")
      CALL dbcsr_data_get_sizes(dense_b, ldb, i, valid)
      IF (.NOT. valid) &
         DBCSR_ABORT("dense matrix not valid")
      !
      !
      m = limits(2) - limits(1) + 1
      n = limits(4) - limits(3) + 1
      k = limits(6) - limits(5) + 1
      a_row = limits(1); a_col = limits(5)
      b_row = limits(5); b_col = limits(3)
      c_row = limits(1); c_col = limits(3)
      !
      !
      IF (transA == dbcsr_no_transpose) THEN
         a_m = m
         a_n = k
      ELSE
         a_m = k
         a_n = m
         i = a_row
         a_row = a_col
         a_col = i
      END IF
      IF (transB == dbcsr_no_transpose) THEN
         b_m = k
         b_n = n
      ELSE
         b_m = n
         b_n = k
         i = b_row
         b_row = b_col
         b_col = i
      END IF
      !
      ! set the size of the work array
      lwork = MAXVAL((/lda, ldb, ldc/))
      !
      !
      SELECT CASE (dense_a%d%data_type)
      CASE (dbcsr_type_real_8_2d)
         ALLOCATE (work(lwork), STAT=istat)
         IF (istat /= 0) &
            DBCSR_ABORT("allocation problem")
         eps = dlamch('eps')
         a_norm = dlange(norm, a_m, a_n, dense_a%d%r2_dp(a_row, a_col), lda, work)
         b_norm = dlange(norm, b_m, b_n, dense_b%d%r2_dp(b_row, b_col), ldb, work)
         c_norm_in = dlange(norm, c_row_size, c_col_size, dense_c%d%r2_dp(1, 1), ldc, work)
         c_norm_dbcsr = dlange(norm, c_row_size, c_col_size, dense_c_dbcsr%d%r2_dp(1, 1), ldc, work)
         !
         CALL dgemm(transa, transb, m, n, k, alpha%r_dp, dense_a%d%r2_dp(a_row, a_col), lda, &
                    dense_b%d%r2_dp(b_row, b_col), ldb, beta%r_dp, dense_c%d%r2_dp(c_row, c_col), ldc)
         !
         ! impose the sparsity if needed
         IF (retain_sparsity) CALL dbcsr_impose_sparsity(matrix_c, dense_c)
         !
         c_norm_out = dlange(norm, m, n, dense_c%d%r2_dp(c_row, c_col), ldc, work)
         !
         ! take the difference dense/sparse
         dense_c%d%r2_dp = dense_c%d%r2_dp - dense_c_dbcsr%d%r2_dp
         !
         ! compute the residual
         residual = dlange(norm, c_row_size, c_col_size, dense_c%d%r2_dp(1, 1), ldc, work)
         DEALLOCATE (work)
      CASE (dbcsr_type_real_4_2d)
         ALLOCATE (work_sp(lwork), STAT=istat)
         IF (istat /= 0) &
            DBCSR_ABORT("allocation problem")
         eps = REAL(slamch('eps'), real_8)
         a_norm = slange(norm, a_m, a_n, dense_a%d%r2_sp(a_row, a_col), lda, work_sp)
         b_norm = slange(norm, b_m, b_n, dense_b%d%r2_sp(b_row, b_col), ldb, work_sp)
         c_norm_in = slange(norm, c_row_size, c_col_size, dense_c%d%r2_sp(1, 1), ldc, work_sp)
         c_norm_dbcsr = slange(norm, c_row_size, c_col_size, dense_c_dbcsr%d%r2_sp(1, 1), ldc, work_sp)
         !

         IF (.FALSE.) THEN
            !IF (io_unit .GT. 0) THEN
            DO j = 1, SIZE(dense_a%d%r2_sp, 2)
               DO i = 1, SIZE(dense_a%d%r2_sp, 1)
                  WRITE (*, '(A,I3,A,I3,A,E15.7,A)') 'a(', i, ',', j, ')=', dense_a%d%r2_sp(i, j), ';'
               END DO
            END DO
            DO j = 1, SIZE(dense_b%d%r2_sp, 2)
               DO i = 1, SIZE(dense_b%d%r2_sp, 1)
                  WRITE (*, '(A,I3,A,I3,A,E15.7,A)') 'b(', i, ',', j, ')=', dense_b%d%r2_sp(i, j), ';'
               END DO
            END DO
            DO j = 1, SIZE(dense_c%d%r2_sp, 2)
               DO i = 1, SIZE(dense_c%d%r2_sp, 1)
                  WRITE (*, '(A,I3,A,I3,A,E15.7,A)') 'c_in(', i, ',', j, ')=', dense_c%d%r2_sp(i, j), ';'
               END DO
            END DO
         END IF

         CALL sgemm(transa, transb, m, n, k, alpha%r_sp, dense_a%d%r2_sp(a_row, a_col), lda, &
                    dense_b%d%r2_sp(b_row, b_col), ldb, beta%r_sp, dense_c%d%r2_sp(c_row, c_col), ldc)
         !
         ! impose the sparsity if needed
         IF (retain_sparsity) CALL dbcsr_impose_sparsity(matrix_c, dense_c)

         IF (.FALSE.) THEN
            !IF (io_unit .GT. 0) THEN
            DO j = 1, SIZE(dense_c%d%r2_sp, 2)
               DO i = 1, SIZE(dense_c%d%r2_sp, 1)
                  WRITE (*, '(A,I3,A,I3,A,E15.7,A)') 'c_out(', i, ',', j, ')=', dense_c%d%r2_sp(i, j), ';'
               END DO
            END DO
            DO j = 1, SIZE(dense_c_dbcsr%d%r2_sp, 2)
               DO i = 1, SIZE(dense_c_dbcsr%d%r2_sp, 1)
                  WRITE (*, '(A,I3,A,I3,A,E15.7,A)') 'c_dbcsr(', i, ',', j, ')=', dense_c_dbcsr%d%r2_sp(i, j), ';'
               END DO
            END DO
         END IF
         !
         c_norm_out = slange(norm, m, n, dense_c%d%r2_sp(c_row, c_col), ldc, work_sp)
         !
         ! take the difference dense/sparse
         dense_c%d%r2_sp = dense_c%d%r2_sp - dense_c_dbcsr%d%r2_sp
         !
         ! compute the residual
         residual = REAL(slange(norm, c_row_size, c_col_size, dense_c%d%r2_sp(1, 1), ldc, work_sp), real_8)
         DEALLOCATE (work_sp)
      CASE (dbcsr_type_complex_8_2d)
         ALLOCATE (work(lwork), STAT=istat)
         IF (istat /= 0) &
            DBCSR_ABORT("allocation problem")
         eps = dlamch('eps')
         a_norm = zlange(norm, a_m, a_n, dense_a%d%c2_dp(a_row, a_col), lda, work)
         b_norm = zlange(norm, b_m, b_n, dense_b%d%c2_dp(b_row, b_col), ldb, work)
         c_norm_in = zlange(norm, c_row_size, c_col_size, dense_c%d%c2_dp(1, 1), ldc, work)
         c_norm_dbcsr = zlange(norm, c_row_size, c_col_size, dense_c_dbcsr%d%c2_dp(1, 1), ldc, work)
         !
         CALL zgemm(transa, transb, m, n, k, alpha%c_dp, dense_a%d%c2_dp(a_row, a_col), lda, &
                    dense_b%d%c2_dp(b_row, b_col), ldb, beta%c_dp, dense_c%d%c2_dp(c_row, c_col), ldc)
         !
         ! impose the sparsity if needed
         IF (retain_sparsity) CALL dbcsr_impose_sparsity(matrix_c, dense_c)
         !
         c_norm_out = zlange(norm, m, n, dense_c%d%c2_dp(c_row, c_col), ldc, work)
         !
         ! take the difference dense/sparse
         dense_c%d%c2_dp = dense_c%d%c2_dp - dense_c_dbcsr%d%c2_dp
         !
         ! compute the residual
         residual = zlange(norm, c_row_size, c_col_size, dense_c%d%c2_dp(1, 1), ldc, work)
         DEALLOCATE (work)
      CASE (dbcsr_type_complex_4_2d)
         ALLOCATE (work_sp(lwork), STAT=istat)
         IF (istat /= 0) &
            DBCSR_ABORT("allocation problem")
         eps = REAL(slamch('eps'), real_8)
         a_norm = clange(norm, a_m, a_n, dense_a%d%c2_sp(a_row, a_col), lda, work_sp)
         b_norm = clange(norm, b_m, b_n, dense_b%d%c2_sp(b_row, b_col), ldb, work_sp)
         c_norm_in = clange(norm, c_row_size, c_col_size, dense_c%d%c2_sp(1, 1), ldc, work_sp)
         c_norm_dbcsr = clange(norm, c_row_size, c_col_size, dense_c_dbcsr%d%c2_sp(1, 1), ldc, work_sp)
         !
         CALL cgemm(transa, transb, m, n, k, alpha%c_sp, dense_a%d%c2_sp(a_row, a_col), lda, &
                    dense_b%d%c2_sp(b_row, b_col), ldb, beta%c_sp, dense_c%d%c2_sp(c_row, c_col), ldc)
         !
         ! impose the sparsity if needed
         IF (retain_sparsity) CALL dbcsr_impose_sparsity(matrix_c, dense_c)
         !
         c_norm_out = clange(norm, m, n, dense_c%d%c2_sp(c_row, c_col), ldc, work_sp)
         !
         ! take the difference dense/sparse
         dense_c%d%c2_sp = dense_c%d%c2_sp - dense_c_dbcsr%d%c2_sp
         !
         ! compute the residual
         residual = clange(norm, c_row_size, c_col_size, dense_c%d%c2_sp(1, 1), ldc, work_sp)
         DEALLOCATE (work_sp)
      CASE default
         DBCSR_ABORT("Incorrect or 1-D data type")
      END SELECT

      IF (mynode .EQ. 0) THEN
         eps_norm = residual/((a_norm + b_norm + c_norm_in)*REAL(n, real_8)*eps)
         IF (eps_norm .GT. 10.0_real_8) THEN
            success = .FALSE.
         ELSE
            success = .TRUE.
         END IF
      END IF
      !
      ! synchronize the result...
      CALL mp_bcast(success, 0, mp_group)
      CALL mp_bcast(eps_norm, 0, mp_group)
      !
      ! printing
      IF (io_unit .GT. 0) THEN
         WRITE (io_unit, *) 'test_name ', test_name
         !
         ! check for nan or inf here
         IF (success) THEN
            WRITE (io_unit, '(A)') ' The solution is CORRECT !'
         ELSE
            WRITE (io_unit, '(A)') ' The solution is suspicious !'

            WRITE (io_unit, '(3(A,E12.5))') ' residual ', residual, ', a_norm ', a_norm, ', b_norm ', b_norm
            WRITE (io_unit, '(3(A,E12.5))') ' c_norm_in ', c_norm_in, ', c_norm_out ', c_norm_out, &
               ', c_norm_dbcsr ', c_norm_dbcsr
            WRITE (io_unit, '(A)') ' Checking the norm of the difference against reference GEMM '
            WRITE (io_unit, '(A,E12.5)') ' -- ||C_dbcsr-C_dense||_oo/((||A||_oo+||B||_oo+||C||_oo).N.eps)=', &
               eps_norm
         END IF

      END IF

      CALL timestop(handle)

   END SUBROUTINE dbcsr_check_multiply

END MODULE dbcsr_test_multiply
