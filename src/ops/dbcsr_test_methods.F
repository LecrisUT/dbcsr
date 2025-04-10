!--------------------------------------------------------------------------------------------------!
! Copyright (C) by the DBCSR developers group - All rights reserved                                !
! This file is part of the DBCSR library.                                                          !
!                                                                                                  !
! For information on the license, see the LICENSE file.                                            !
! For further information please visit https://dbcsr.cp2k.org                                      !
! SPDX-License-Identifier: GPL-2.0+                                                                !
!--------------------------------------------------------------------------------------------------!

MODULE dbcsr_test_methods
   !! Tests for CP2K DBCSR operations
   USE dbcsr_blas_operations, ONLY: dbcsr_lapack_larnv, &
                                    set_larnv_seed
   USE dbcsr_block_access, ONLY: dbcsr_put_block
   USE dbcsr_block_operations, ONLY: dbcsr_block_conjg, &
                                     dbcsr_block_partial_copy, &
                                     dbcsr_block_scale, &
                                     dbcsr_block_transpose, &
                                     dbcsr_data_clear, &
                                     dbcsr_data_set
   USE dbcsr_data_methods, ONLY: &
      dbcsr_data_clear_pointer, dbcsr_data_get_sizes, dbcsr_data_get_type, dbcsr_data_init, &
      dbcsr_data_new, dbcsr_data_release, dbcsr_scalar, dbcsr_scalar_negative, dbcsr_scalar_one, &
      dbcsr_type_1d_to_2d, dbcsr_type_2d_to_1d
   USE dbcsr_dist_methods, ONLY: dbcsr_distribution_hold, &
                                 dbcsr_distribution_mp, &
                                 dbcsr_distribution_new, &
                                 dbcsr_distribution_release
   USE dbcsr_dist_operations, ONLY: dbcsr_get_stored_coordinates
   USE dbcsr_dist_util, ONLY: dbcsr_verify_matrix
   USE dbcsr_iterator_operations, ONLY: dbcsr_iterator_blocks_left, &
                                        dbcsr_iterator_next_block, &
                                        dbcsr_iterator_start, &
                                        dbcsr_iterator_stop
   USE dbcsr_kinds, ONLY: dp, &
                          int_8, &
                          real_4, &
                          real_8
   USE dbcsr_methods, ONLY: dbcsr_get_matrix_type, &
                            dbcsr_max_col_size, &
                            dbcsr_max_row_size, &
                            dbcsr_nblkcols_total, &
                            dbcsr_nblkrows_total, &
                            dbcsr_nfullcols_total, &
                            dbcsr_nfullrows_total
   USE dbcsr_mp_methods, ONLY: dbcsr_mp_mynode, &
                               dbcsr_mp_new, &
                               dbcsr_mp_numnodes, &
                               dbcsr_mp_release
   USE dbcsr_mpiwrap, ONLY: mp_comm_null, &
                            mp_environ, mp_comm_type
   USE dbcsr_ptr_util, ONLY: ensure_array_size
   USE dbcsr_types, ONLY: &
      dbcsr_data_obj, dbcsr_distribution_obj, dbcsr_iterator, dbcsr_mp_obj, dbcsr_scalar_type, &
      dbcsr_type, dbcsr_type_antihermitian, dbcsr_type_antisymmetric, dbcsr_type_complex_4, &
      dbcsr_type_complex_8, dbcsr_type_hermitian, dbcsr_type_no_symmetry, dbcsr_type_real_4, &
      dbcsr_type_real_8, dbcsr_type_real_default, dbcsr_type_symmetric
   USE dbcsr_work_operations, ONLY: dbcsr_create, &
                                    dbcsr_finalize, &
                                    dbcsr_work_create
#include "base/dbcsr_base_uses.f90"

!$ USE OMP_LIB, ONLY: omp_get_max_threads, omp_get_thread_num, omp_get_num_threads

   IMPLICIT NONE

   PRIVATE

   PUBLIC :: dbcsr_to_dense_local, dbcsr_impose_sparsity
   PUBLIC :: dbcsr_random_dist, dbcsr_make_random_matrix, &
             dbcsr_make_random_block_sizes, compx_to_dbcsr_scalar, &
             dbcsr_reset_randmat_seed

   CHARACTER(len=*), PARAMETER, PRIVATE :: moduleN = 'dbcsr_test_methods'

   INTEGER, PRIVATE, SAVE :: randmat_counter = 0
   INTEGER, PARAMETER, PRIVATE :: rand_seed_init = 12341313

CONTAINS

   SUBROUTINE dbcsr_reset_randmat_seed()
      !! Reset the seed used for generating random matrices to default value
      randmat_counter = rand_seed_init
   END SUBROUTINE

   FUNCTION compx_to_dbcsr_scalar(z, data_type) RESULT(res)
      COMPLEX(real_8)                                    :: z
      INTEGER                                            :: data_type
      TYPE(dbcsr_scalar_type)                            :: res

      SELECT CASE (data_type)
      CASE (dbcsr_type_real_4)
         res = dbcsr_scalar(REAL(z, kind=real_4))
      CASE (dbcsr_type_real_8)
         res = dbcsr_scalar(REAL(z, kind=real_8))
      CASE (dbcsr_type_complex_4)
         res = dbcsr_scalar(CMPLX(z, kind=real_4))
      CASE (dbcsr_type_complex_8)
         res = dbcsr_scalar(z)
      END SELECT

   END FUNCTION compx_to_dbcsr_scalar

   SUBROUTINE dbcsr_impose_sparsity(sparse, dense)
      !! Impose sparsity on a dense matrix based on a dbcsr

      TYPE(dbcsr_type), INTENT(IN)                       :: sparse
         !! sparse matrix
      TYPE(dbcsr_data_obj), INTENT(inout)                :: dense
         !! dense matrix Take into account the symmetry of the sparse matrix. The dense matrix need to be valid. The operation is
         !! done locally.

      CHARACTER(len=*), PARAMETER :: routineN = 'dbcsr_impose_sparsity'

      CHARACTER                                          :: symm
      INTEGER                                            :: blk, col, col_offset, col_size, &
                                                            data_type, dense_col_size, &
                                                            dense_row_size, handle, row, &
                                                            row_offset, row_size
      LOGICAL                                            :: valid
      TYPE(dbcsr_data_obj)                               :: tmp
      TYPE(dbcsr_iterator)                               :: iter

      CALL timeset(routineN, handle)

      CALL dbcsr_data_get_sizes(dense, dense_row_size, dense_col_size, valid)
      IF (.NOT. valid) &
         DBCSR_ABORT("dense matrix not valid")
      data_type = dbcsr_data_get_type(dense)
      symm = dbcsr_get_matrix_type(sparse)

      CALL dbcsr_data_init(tmp)
      CALL dbcsr_data_new(tmp, dbcsr_type_1d_to_2d(data_type), data_size=dense_row_size, &
                          data_size2=dense_col_size)
      CALL dbcsr_data_set(dst=tmp, lb=1, data_size=dense_row_size, src=dense, source_lb=1, &
                          lb2=1, data_size2=dense_col_size, source_lb2=1)
      CALL dbcsr_data_clear(dense)

      CALL dbcsr_iterator_start(iter, sparse)
      DO WHILE (dbcsr_iterator_blocks_left(iter))
         CALL dbcsr_iterator_next_block(iter, row, col, blk, &
                                        row_size=row_size, col_size=col_size, &
                                        row_offset=row_offset, col_offset=col_offset)
         CALL dbcsr_block_partial_copy( &
            dst=dense, &
            dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.FALSE., &
            dst_r_lb=row_offset, dst_c_lb=col_offset, &
            src=tmp, &
            src_rs=dense_row_size, src_cs=dense_col_size, src_tr=.FALSE., &
            src_r_lb=row_offset, src_c_lb=col_offset, &
            nrow=row_size, ncol=col_size)
         IF (symm .NE. dbcsr_type_no_symmetry) THEN
            SELECT CASE (symm)
            CASE (dbcsr_type_symmetric)
               CALL dbcsr_block_partial_copy( &
                  dst=dense, &
                  dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.TRUE., &
                  dst_r_lb=row_offset, dst_c_lb=col_offset, &
                  src=tmp, &
                  src_rs=dense_row_size, src_cs=dense_col_size, src_tr=.FALSE., &
                  src_r_lb=row_offset, src_c_lb=col_offset, &
                  nrow=row_size, ncol=col_size)
            CASE (dbcsr_type_antisymmetric)
               CALL dbcsr_block_partial_copy( &
                  dst=dense, &
                  dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.TRUE., &
                  dst_r_lb=row_offset, dst_c_lb=col_offset, &
                  src=tmp, &
                  src_rs=dense_row_size, src_cs=dense_col_size, src_tr=.FALSE., &
                  src_r_lb=row_offset, src_c_lb=col_offset, &
                  nrow=row_size, ncol=col_size)
               CALL dbcsr_block_scale(dense, dbcsr_scalar_negative(dbcsr_scalar_one( &
                                                                   dbcsr_type_2d_to_1d(data_type))), &
                                      row_size=col_size, col_size=row_size, &
                                      lb=col_offset, lb2=row_offset)
            CASE (dbcsr_type_hermitian)
               CALL dbcsr_block_partial_copy( &
                  dst=dense, &
                  dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.TRUE., &
                  dst_r_lb=row_offset, dst_c_lb=col_offset, &
                  src=tmp, &
                  src_rs=dense_row_size, src_cs=dense_col_size, src_tr=.FALSE., &
                  src_r_lb=row_offset, src_c_lb=col_offset, &
                  nrow=row_size, ncol=col_size)
               CALL dbcsr_block_conjg(dense, row_size=col_size, col_size=row_size, &
                                      lb=col_offset, lb2=row_offset)
            CASE (dbcsr_type_antihermitian)
               CALL dbcsr_block_partial_copy( &
                  dst=dense, &
                  dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.TRUE., &
                  dst_r_lb=row_offset, dst_c_lb=col_offset, &
                  src=tmp, &
                  src_rs=dense_row_size, src_cs=dense_col_size, src_tr=.FALSE., &
                  src_r_lb=row_offset, src_c_lb=col_offset, &
                  nrow=row_size, ncol=col_size)
               CALL dbcsr_block_scale(dense, dbcsr_scalar_negative(dbcsr_scalar_one( &
                                                                   dbcsr_type_2d_to_1d(data_type))), &
                                      row_size=col_size, col_size=row_size, &
                                      lb=col_offset, lb2=row_offset)
               CALL dbcsr_block_conjg(dense, row_size=col_size, col_size=row_size, &
                                      lb=col_offset, lb2=row_offset)
            CASE DEFAULT
               DBCSR_ABORT("wrong matrix symmetry")
            END SELECT
         END IF
      END DO
      CALL dbcsr_iterator_stop(iter)

      CALL dbcsr_data_release(tmp)

      CALL timestop(handle)

   END SUBROUTINE dbcsr_impose_sparsity

   SUBROUTINE dbcsr_to_dense_local(sparse, dense)
      !! Convert a sparse matrix to a dense matrix

      TYPE(dbcsr_type), INTENT(in)                       :: sparse
         !! sparse matrix
      TYPE(dbcsr_data_obj), INTENT(inout)                :: dense
         !! dense matrix Take into account the symmetry of the sparse matrix. The dense matrix need to be valid. The operation is
         !! done locally.

      CHARACTER(len=*), PARAMETER :: routineN = 'dbcsr_to_dense_local'

      CHARACTER                                          :: symm
      INTEGER                                            :: col, col_offset, col_size, data_type, &
                                                            dense_col_size, dense_row_size, &
                                                            handle, row, row_offset, row_size
      LOGICAL                                            :: tr, valid
      TYPE(dbcsr_data_obj)                               :: block
      TYPE(dbcsr_iterator)                               :: iter

      CALL timeset(routineN, handle)

      CALL dbcsr_data_get_sizes(dense, dense_row_size, dense_col_size, valid)
      IF (.NOT. valid) &
         DBCSR_ABORT("dense matrix not valid")

      symm = dbcsr_get_matrix_type(sparse)
      data_type = dbcsr_data_get_type(dense)

      CALL dbcsr_data_clear(dense)
      CALL dbcsr_data_init(block)
      CALL dbcsr_data_new(block, dbcsr_type_1d_to_2d(data_type))
      CALL dbcsr_iterator_start(iter, sparse)
      DO WHILE (dbcsr_iterator_blocks_left(iter))
         CALL dbcsr_iterator_next_block(iter, row, col, block, tr, &
                                        row_size=row_size, col_size=col_size, &
                                        row_offset=row_offset, col_offset=col_offset)
         CALL dbcsr_block_partial_copy(dst=dense, &
                                       dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.FALSE., &
                                       dst_r_lb=row_offset, dst_c_lb=col_offset, &
                                       src=block, src_rs=row_size, src_cs=col_size, src_tr=tr, &
                                       src_r_lb=1, src_c_lb=1, nrow=row_size, ncol=col_size)
         IF (symm .NE. dbcsr_type_no_symmetry .AND. row .NE. col) THEN
            SELECT CASE (symm)
            CASE (dbcsr_type_symmetric)
               CALL dbcsr_block_partial_copy(dst=dense, &
                                             dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.TRUE., &
                                             dst_r_lb=row_offset, dst_c_lb=col_offset, &
                                             src=block, src_rs=row_size, src_cs=col_size, src_tr=tr, &
                                             src_r_lb=1, src_c_lb=1, nrow=row_size, ncol=col_size)
            CASE (dbcsr_type_antisymmetric)
               CALL dbcsr_block_partial_copy(dst=dense, &
                                             dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.TRUE., &
                                             dst_r_lb=row_offset, dst_c_lb=col_offset, &
                                             src=block, src_rs=row_size, src_cs=col_size, src_tr=tr, &
                                             src_r_lb=1, src_c_lb=1, nrow=row_size, ncol=col_size)
               CALL dbcsr_block_scale(dense, dbcsr_scalar_negative(dbcsr_scalar_one( &
                                                                   dbcsr_type_2d_to_1d(data_type))), &
                                      row_size=col_size, col_size=row_size, &
                                      lb=col_offset, lb2=row_offset)
            CASE (dbcsr_type_hermitian)
               CALL dbcsr_block_partial_copy(dst=dense, &
                                             dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.TRUE., &
                                             dst_r_lb=row_offset, dst_c_lb=col_offset, &
                                             src=block, src_rs=row_size, src_cs=col_size, src_tr=tr, &
                                             src_r_lb=1, src_c_lb=1, nrow=row_size, ncol=col_size)
               CALL dbcsr_block_conjg(dense, row_size=col_size, col_size=row_size, &
                                      lb=col_offset, lb2=row_offset)
            CASE (dbcsr_type_antihermitian)
               CALL dbcsr_block_partial_copy(dst=dense, &
                                             dst_rs=dense_row_size, dst_cs=dense_col_size, dst_tr=.TRUE., &
                                             dst_r_lb=row_offset, dst_c_lb=col_offset, &
                                             src=block, src_rs=row_size, src_cs=col_size, src_tr=tr, &
                                             src_r_lb=1, src_c_lb=1, nrow=row_size, ncol=col_size)
               CALL dbcsr_block_scale(dense, dbcsr_scalar_negative(dbcsr_scalar_one( &
                                                                   dbcsr_type_2d_to_1d(data_type))), &
                                      row_size=col_size, col_size=row_size, &
                                      lb=col_offset, lb2=row_offset)
               CALL dbcsr_block_conjg(dense, row_size=col_size, col_size=row_size, &
                                      lb=col_offset, lb2=row_offset)
            CASE DEFAULT
               DBCSR_ABORT("wrong matrix symmetry")
            END SELECT
         END IF
      END DO
      CALL dbcsr_iterator_stop(iter)
      CALL dbcsr_data_clear_pointer(block)
      CALL dbcsr_data_release(block)

      CALL timestop(handle)

   END SUBROUTINE dbcsr_to_dense_local

   SUBROUTINE dbcsr_random_dist(dist_array, dist_size, nbins)
      INTEGER, DIMENSION(:), INTENT(out), POINTER        :: dist_array
      INTEGER, INTENT(in)                                :: dist_size, nbins

      INTEGER                                            :: i

      ALLOCATE (dist_array(dist_size))
      !CALL RANDOM_NUMBER (dist_array)
      DO i = 1, dist_size
         dist_array(i) = MODULO(nbins - i, nbins)
      END DO
   END SUBROUTINE dbcsr_random_dist

   SUBROUTINE dbcsr_make_random_matrix(matrix, row_blk_sizes, col_blk_sizes, &
                                       name, sparsity, mp_group, data_type, symmetry, dist)
      !! Creates a random matrix.
      TYPE(dbcsr_type), INTENT(out)                      :: matrix
      INTEGER, DIMENSION(:), INTENT(INOUT), POINTER, CONTIGUOUS :: row_blk_sizes, col_blk_sizes
      CHARACTER(len=*), INTENT(in)                       :: name
      REAL(kind=real_8), INTENT(in)                      :: sparsity
      TYPE(mp_comm_type), INTENT(in)                     :: mp_group
      INTEGER, INTENT(in), OPTIONAL                      :: data_type
      CHARACTER, INTENT(in), OPTIONAL                    :: symmetry
      TYPE(dbcsr_distribution_obj), INTENT(IN), OPTIONAL :: dist

      CHARACTER(len=*), PARAMETER :: routineN = 'dbcsr_make_random_matrix'

      CHARACTER                                          :: my_symmetry
      INTEGER                                            :: col, error_handle, max_nze, &
                                                            my_data_type, my_proc, ncol, nrow, &
                                                            numproc, nze, p, row, s_col, s_row
      INTEGER(KIND=int_8)                                :: counter, ele, increment, nmax
      INTEGER, DIMENSION(4)                              :: iseed, jseed
      LOGICAL                                            :: tr
      REAL(kind=real_8)                                  :: my_sparsity
      REAL(kind=real_8), DIMENSION(1)                    :: value
      TYPE(dbcsr_data_obj)                               :: data_values, data_values_tr
      TYPE(dbcsr_distribution_obj)                       :: new_dist

!   ---------------------------------------------------------------------------

      CALL timeset(routineN, error_handle)
      ! Check that the counter was initialised (or has not overflowed)
      DBCSR_ASSERT(randmat_counter .NE. 0)
      ! the counter goes into the seed. Every new call gives a new random matrix
      randmat_counter = randmat_counter + 1
      ! Create the matrix
      IF (PRESENT(dist)) THEN
         new_dist = dist
         CALL dbcsr_distribution_hold(new_dist)
      ELSE
         CALL dbcsr_make_null_dist(new_dist, SIZE(row_blk_sizes), &
                                   SIZE(col_blk_sizes), group=mp_group)
      END IF
      my_data_type = dbcsr_type_real_default
      IF (PRESENT(data_type)) my_data_type = data_type
      my_symmetry = dbcsr_type_no_symmetry
      IF (PRESENT(symmetry)) my_symmetry = symmetry
      CALL dbcsr_create(matrix, name, &
                        new_dist, my_symmetry, &
                        row_blk_sizes, &
                        col_blk_sizes, &
                        data_type=my_data_type)
      numproc = dbcsr_mp_numnodes(dbcsr_distribution_mp(new_dist))
      my_proc = dbcsr_mp_mynode(dbcsr_distribution_mp(new_dist))
      !
      IF (sparsity .GT. 1) THEN
         my_sparsity = sparsity/100.0
      ELSE
         my_sparsity = sparsity
      END IF
      CALL dbcsr_work_create(matrix, &
                             nblks_guess=INT(REAL(dbcsr_nblkrows_total(matrix), KIND=dp) &
                                             *REAL(dbcsr_nblkcols_total(matrix), KIND=dp) &
                                             *(1.0_dp - sparsity)*1.1_dp/numproc), &
                             sizedata_guess=INT(REAL(dbcsr_nfullrows_total(matrix), KIND=dp) &
                                                *REAL(dbcsr_nfullcols_total(matrix), KIND=dp) &
                                                *(1.0_dp - sparsity)*1.1_dp/numproc), &
                             work_mutable=.TRUE.)

      max_nze = dbcsr_max_row_size(matrix)*dbcsr_max_col_size(matrix)
      CALL dbcsr_data_init(data_values)
      CALL dbcsr_data_new(data_values, my_data_type, data_size=max_nze)
      CALL dbcsr_data_init(data_values_tr)
      CALL dbcsr_data_new(data_values_tr, my_data_type, data_size=max_nze)

      nrow = dbcsr_nblkrows_total(matrix)
      ncol = dbcsr_nblkcols_total(matrix)
      nmax = INT(nrow, KIND=int_8)*INT(ncol, KIND=int_8)
      ele = -1
      counter = 0
      CALL set_larnv_seed(7, 42, 3, 42, randmat_counter, jseed)

      DO
         ! find the next block to add, this is given by a geometrically distributed variable
         ! we number the blocks of the matrix and jump to the next one
         CALL dlarnv(1, jseed, 1, value)
         IF (my_sparsity > 0) THEN
            increment = 1 + FLOOR(LOG(value(1))/LOG(my_sparsity), KIND=int_8)
         ELSE
            increment = 1
         END IF
         ele = ele + increment
         IF (ele >= nmax) EXIT
         counter = counter + 1
         row = INT(ele/ncol) + 1
         col = INT(MOD(ele, INT(ncol, KIND=KIND(ele)))) + 1

         ! build the upper matrix if some symmetry, and only deal with the local blocks.
         s_row = row; s_col = col
         IF (PRESENT(dist)) THEN
            tr = .FALSE.
            CALL dbcsr_get_stored_coordinates(matrix, s_row, s_col, p)
            IF (my_symmetry .NE. dbcsr_type_no_symmetry .AND. s_col .LT. s_row) CYCLE
            IF (p .NE. my_proc) CYCLE
         ELSE
            IF (my_symmetry .NE. dbcsr_type_no_symmetry .AND. s_col .LT. s_row) CYCLE
         END IF
         IF (.NOT. PRESENT(dist) .AND. my_proc .NE. 0) CYCLE

         ! fill based on a block based seed, makes this the same values in parallel
         CALL set_larnv_seed(row, nrow, col, ncol, randmat_counter, iseed)
         nze = row_blk_sizes(s_row)*col_blk_sizes(s_col)
         CALL dbcsr_lapack_larnv(1, iseed, nze, data_values)
         CALL dbcsr_put_block(matrix, s_row, s_col, data_values)
         IF (my_symmetry .NE. dbcsr_type_no_symmetry .AND. s_col .EQ. s_row) THEN
            SELECT CASE (my_symmetry)
            CASE (dbcsr_type_symmetric)
               CALL dbcsr_block_transpose(data_values_tr, data_values, &
                                          row_size=row_blk_sizes(s_row), col_size=col_blk_sizes(s_col), lb=1, source_lb=1)
            CASE (dbcsr_type_antisymmetric)
               CALL dbcsr_block_transpose(data_values_tr, data_values, &
                                          row_size=row_blk_sizes(s_row), col_size=col_blk_sizes(s_col), lb=1, source_lb=1, &
                                          scale=dbcsr_scalar_negative(dbcsr_scalar_one(my_data_type)))
            CASE (dbcsr_type_hermitian)
               CALL dbcsr_block_transpose(data_values_tr, data_values, &
                                          row_size=row_blk_sizes(s_row), col_size=col_blk_sizes(s_col), lb=1, source_lb=1)
               CALL dbcsr_block_conjg(data_values_tr, row_size=col_blk_sizes(s_col), col_size=row_blk_sizes(s_row), &
                                      lb=1)
            CASE (dbcsr_type_antihermitian)
               CALL dbcsr_block_transpose(data_values_tr, data_values, &
                                          row_size=row_blk_sizes(s_row), col_size=col_blk_sizes(s_col), lb=1, source_lb=1, &
                                          scale=dbcsr_scalar_negative(dbcsr_scalar_one(my_data_type)))
               CALL dbcsr_block_conjg(data_values_tr, row_size=col_blk_sizes(s_col), col_size=row_blk_sizes(s_row), &
                                      lb=1)
            CASE DEFAULT
               DBCSR_ABORT("wrong matrix symmetry")
            END SELECT
            CALL dbcsr_put_block(matrix, s_row, s_col, data_values_tr, summation=.TRUE.)
         END IF
      END DO

      CALL dbcsr_data_release(data_values)
      CALL dbcsr_data_release(data_values_tr)

      CALL dbcsr_distribution_release(new_dist)
      CALL dbcsr_finalize(matrix)
      CALL dbcsr_verify_matrix(matrix)
      !
      CALL timestop(error_handle)
   END SUBROUTINE dbcsr_make_random_matrix

   SUBROUTINE dbcsr_make_random_block_sizes(block_sizes, size_sum, size_mix)
      INTEGER, DIMENSION(:), INTENT(out), POINTER        :: block_sizes
      INTEGER, INTENT(in)                                :: size_sum
      INTEGER, DIMENSION(:), INTENT(in)                  :: size_mix

      INTEGER                                            :: block_size, current_sum, nblocks, &
                                                            nsize_mix, selector
      INTEGER, ALLOCATABLE, DIMENSION(:, :)              :: mixer
      INTEGER, DIMENSION(:), POINTER, CONTIGUOUS         :: sizes

!

      NULLIFY (sizes)
      nsize_mix = SIZE(size_mix)/2
      ALLOCATE (mixer(3, nsize_mix))
      mixer(1, :) = size_mix(1:nsize_mix*2 - 1:2)
      mixer(2, :) = size_mix(2:nsize_mix*2:2)
      mixer(3, :) = 1
      nblocks = 0
      current_sum = 0
      CALL ensure_array_size(sizes, lb=1, ub=1)

      selector = 1
      !
      DO WHILE (current_sum .LT. size_sum)
         nblocks = nblocks + 1
         !CALL RANDOM_NUMBER(value)
         !block_size = MIN (INT (value(1) * size_max),&
         !                  size_sum - current_sum)
         block_size = MIN(mixer(2, selector), &
                          size_sum - current_sum)
         sizes(nblocks) = block_size
         current_sum = current_sum + block_size
         CALL ensure_array_size(sizes, ub=nblocks + 1, factor=2.0_dp)
         mixer(3, selector) = mixer(3, selector) + 1
         IF (mixer(3, selector) .GT. mixer(1, selector)) THEN
            mixer(3, selector) = 1
            selector = MOD(selector, nsize_mix) + 1
         END IF
      END DO
      ALLOCATE (block_sizes(nblocks))
      block_sizes = sizes(1:nblocks)
      current_sum = SUM(block_sizes)
      IF (current_sum /= size_sum) &
         DBCSR_ABORT("Incorrect block sizes")
      DEALLOCATE (mixer, sizes)

   END SUBROUTINE dbcsr_make_random_block_sizes

   SUBROUTINE dbcsr_make_null_mp(mp_env, group)
      TYPE(dbcsr_mp_obj), INTENT(out)                    :: mp_env
      TYPE(mp_comm_type), INTENT(in), OPTIONAL           :: group

      INTEGER                                            :: mynode, numnodes

      IF (PRESENT(group)) THEN
         CALL mp_environ(numnodes, mynode, group)
         CALL dbcsr_mp_new(mp_env, group, &
                           RESHAPE((/1/), (/1, 1/)), &
                           mynode, numnodes, &
                           myprow=0, mypcol=0)
      ELSE
         CALL dbcsr_mp_new(mp_env, MP_COMM_NULL, &
                           RESHAPE((/1/), (/1, 1/)), &
                           0, 1, &
                           myprow=0, mypcol=0)
      END IF
   END SUBROUTINE dbcsr_make_null_mp
   !
   SUBROUTINE dbcsr_make_null_dist(distribution, nblkrows, nblkcols, group)
      TYPE(dbcsr_distribution_obj), INTENT(out)          :: distribution
      INTEGER, INTENT(in)                                :: nblkrows, nblkcols
      TYPE(mp_comm_type), INTENT(in), OPTIONAL           :: group

      INTEGER, DIMENSION(:), POINTER, CONTIGUOUS         :: col_dist, row_dist
      TYPE(dbcsr_mp_obj)                                 :: mp_env

      CALL dbcsr_make_null_mp(mp_env, group=group)
      ALLOCATE (row_dist(nblkrows), col_dist(nblkcols))
      row_dist = 0
      col_dist = 0
      CALL dbcsr_distribution_new(distribution, mp_env, &
                                  row_dist, col_dist, reuse_arrays=.TRUE.)
      CALL dbcsr_mp_release(mp_env)
   END SUBROUTINE dbcsr_make_null_dist

END MODULE dbcsr_test_methods
