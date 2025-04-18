!--------------------------------------------------------------------------------------------------!
! Copyright (C) by the DBCSR developers group - All rights reserved                                !
! This file is part of the DBCSR library.                                                          !
!                                                                                                  !
! For information on the license, see the LICENSE file.                                            !
! For further information please visit https://dbcsr.cp2k.org                                      !
! SPDX-License-Identifier: GPL-2.0+                                                                !
!--------------------------------------------------------------------------------------------------!

MODULE dbcsr_tensor_io
   !! DBCSR tensor Input / Output

   #:include "dbcsr_tensor.fypp"
   #:set maxdim = maxrank
   #:set ndims = range(2,maxdim+1)

   USE dbcsr_tensor_types, ONLY: &
      dbcsr_t_get_info, dbcsr_t_type, ndims_tensor, dbcsr_t_get_num_blocks, dbcsr_t_get_num_blocks_total, &
      blk_dims_tensor, dbcsr_t_get_stored_coordinates, dbcsr_t_get_nze, dbcsr_t_get_nze_total, &
      dbcsr_t_pgrid_type, dbcsr_t_nblks_total
   USE dbcsr_kinds, ONLY: default_string_length, int_8, real_8
   USE dbcsr_mpiwrap, ONLY: mp_environ, mp_max, mp_comm_type
   USE dbcsr_tensor_block, ONLY: &
      dbcsr_t_iterator_type, dbcsr_t_iterator_next_block, dbcsr_t_iterator_start, &
      dbcsr_t_iterator_blocks_left, dbcsr_t_iterator_stop, dbcsr_t_get_block
   USE dbcsr_tas_io, ONLY: dbcsr_tas_write_split_info

#include "base/dbcsr_base_uses.f90"

   IMPLICIT NONE
   PRIVATE
   CHARACTER(len=*), PARAMETER, PRIVATE :: moduleN = 'dbcsr_tensor_types'

   PUBLIC :: &
      dbcsr_t_write_tensor_info, &
      dbcsr_t_write_tensor_dist, &
      dbcsr_t_write_blocks, &
      dbcsr_t_write_block, &
      dbcsr_t_write_block_indices, &
      dbcsr_t_write_split_info, &
      prep_output_unit

CONTAINS

   SUBROUTINE dbcsr_t_write_tensor_info(tensor, unit_nr, full_info)
      !! Write tensor global info: block dimensions, full dimensions and process grid dimensions

      TYPE(dbcsr_t_type), INTENT(IN) :: tensor
      INTEGER, INTENT(IN)            :: unit_nr
      LOGICAL, OPTIONAL, INTENT(IN)  :: full_info
         !! Whether to print distribution and block size vectors
      INTEGER, DIMENSION(ndims_tensor(tensor)) :: nblks_total, nfull_total, pdims, my_ploc, nblks_local, nfull_local

      #:for idim in range(1, maxdim+1)
         INTEGER, DIMENSION(dbcsr_t_nblks_total(tensor, ${idim}$)) :: proc_dist_${idim}$
         INTEGER, DIMENSION(dbcsr_t_nblks_total(tensor, ${idim}$)) :: blk_size_${idim}$
         INTEGER, DIMENSION(dbcsr_t_nblks_total(tensor, ${idim}$)) :: blks_local_${idim}$
      #:endfor
      CHARACTER(len=default_string_length)                     :: name
      INTEGER                                                  :: idim
      INTEGER                                                  :: iblk
      INTEGER                                                  :: unit_nr_prv

      unit_nr_prv = prep_output_unit(unit_nr)
      IF (unit_nr_prv == 0) RETURN

      CALL dbcsr_t_get_info(tensor, nblks_total, nfull_total, nblks_local, nfull_local, pdims, my_ploc, &
                            ${varlist("blks_local")}$, ${varlist("proc_dist")}$, ${varlist("blk_size")}$, &
                            name=name)

      IF (unit_nr_prv > 0) THEN
         WRITE (unit_nr_prv, "(T2,A)") &
            "GLOBAL INFO OF "//TRIM(name)
         WRITE (unit_nr_prv, "(T4,A,1X)", advance="no") "block dimensions:"
         DO idim = 1, ndims_tensor(tensor)
            WRITE (unit_nr_prv, "(I6)", advance="no") nblks_total(idim)
         END DO
         WRITE (unit_nr_prv, "(/T4,A,1X)", advance="no") "full dimensions:"
         DO idim = 1, ndims_tensor(tensor)
            WRITE (unit_nr_prv, "(I8)", advance="no") nfull_total(idim)
         END DO
         WRITE (unit_nr_prv, "(/T4,A,1X)", advance="no") "process grid dimensions:"
         DO idim = 1, ndims_tensor(tensor)
            WRITE (unit_nr_prv, "(I6)", advance="no") pdims(idim)
         END DO
         WRITE (unit_nr_prv, *)

         IF (PRESENT(full_info)) THEN
            IF (full_info) THEN
               WRITE (unit_nr_prv, '(T4,A)', advance='no') "Block sizes:"
               #:for dim in range(1, maxdim+1)
                  IF (ndims_tensor(tensor) >= ${dim}$) THEN
                     WRITE (unit_nr_prv, '(/T8,A,1X,I1,A,1X)', advance='no') 'Dim', ${dim}$, ':'
                     DO iblk = 1, SIZE(blk_size_${dim}$)
                        WRITE (unit_nr_prv, '(I2,1X)', advance='no') blk_size_${dim}$ (iblk)
                     END DO
                  END IF
               #:endfor
               WRITE (unit_nr_prv, '(/T4,A)', advance='no') "Block distribution:"
               #:for dim in range(1, maxdim+1)
                  IF (ndims_tensor(tensor) >= ${dim}$) THEN
                     WRITE (unit_nr_prv, '(/T8,A,1X,I1,A,1X)', advance='no') 'Dim', ${dim}$, ':'
                     DO iblk = 1, SIZE(proc_dist_${dim}$)
                        WRITE (unit_nr_prv, '(I3,1X)', advance='no') proc_dist_${dim}$ (iblk)
                     END DO
                  END IF
               #:endfor
            END IF
            WRITE (unit_nr_prv, *)
         END IF
      END IF

   END SUBROUTINE

   SUBROUTINE dbcsr_t_write_tensor_dist(tensor, unit_nr)
      !! Write info on tensor distribution & load balance
      TYPE(dbcsr_t_type), INTENT(IN) :: tensor
      INTEGER, INTENT(IN)            :: unit_nr
      INTEGER                        :: nproc, myproc, nblock_max, nelement_max
      INTEGER(KIND=int_8)            :: nblock_sum, nelement_sum, nblock_tot
      INTEGER                        :: nblock, nelement, unit_nr_prv
      TYPE(mp_comm_type)             :: mp_comm
      INTEGER, DIMENSION(2)          :: tmp
      INTEGER, DIMENSION(ndims_tensor(tensor)) :: bdims
      REAL(KIND=real_8)              :: occupation

      mp_comm = tensor%pgrid%mp_comm_2d
      unit_nr_prv = prep_output_unit(unit_nr)
      IF (unit_nr_prv == 0) RETURN

      CALL mp_environ(nproc, myproc, mp_comm)

      nblock = dbcsr_t_get_num_blocks(tensor)
      nelement = dbcsr_t_get_nze(tensor)

      nblock_sum = dbcsr_t_get_num_blocks_total(tensor)
      nelement_sum = dbcsr_t_get_nze_total(tensor)

      tmp = (/nblock, nelement/)
      CALL mp_max(tmp, mp_comm)
      nblock_max = tmp(1); nelement_max = tmp(2)

      CALL blk_dims_tensor(tensor, bdims)
      nblock_tot = PRODUCT(INT(bdims, KIND=int_8))

      occupation = -1.0_real_8
      IF (nblock_tot .NE. 0) occupation = 100.0_real_8*REAL(nblock_sum, real_8)/REAL(nblock_tot, real_8)

      IF (unit_nr_prv > 0) THEN
         WRITE (unit_nr_prv, "(T2,A)") &
            "DISTRIBUTION OF "//TRIM(tensor%name)
         WRITE (unit_nr_prv, "(T15,A,T68,I13)") "Number of non-zero blocks:", nblock_sum
         WRITE (unit_nr_prv, "(T15,A,T75,F6.2)") "Percentage of non-zero blocks:", occupation
         WRITE (unit_nr_prv, "(T15,A,T68,I13)") "Average number of blocks per CPU:", (nblock_sum + nproc - 1)/nproc
         WRITE (unit_nr_prv, "(T15,A,T68,I13)") "Maximum number of blocks per CPU:", nblock_max
         WRITE (unit_nr_prv, "(T15,A,T68,I13)") "Average number of matrix elements per CPU:", (nelement_sum + nproc - 1)/nproc
         WRITE (unit_nr_prv, "(T15,A,T68,I13)") "Maximum number of matrix elements per CPU:", nelement_max
      END IF

   END SUBROUTINE

   SUBROUTINE dbcsr_t_write_blocks(tensor, io_unit_master, io_unit_all, write_int)
      !! Write all tensor blocks

      TYPE(dbcsr_t_type), INTENT(INOUT)                  :: tensor
      INTEGER, INTENT(IN)                                :: io_unit_master, io_unit_all
         !! for global output
         !! for local output
      LOGICAL, INTENT(IN), OPTIONAL                      :: write_int
         !! convert to integers (useful for testing with integer tensors)
      INTEGER                                            :: blk
      INTEGER, DIMENSION(ndims_tensor(tensor))          :: blk_index, blk_size
      #:for ndim in ndims
         REAL(KIND=real_8), ALLOCATABLE, &
            DIMENSION(${shape_colon(ndim)}$)                :: blk_values_${ndim}$
      #:endfor
      TYPE(dbcsr_t_iterator_type)                        :: iterator
      INTEGER                                            :: proc, mynode, numnodes
      LOGICAL                                            :: found

      IF (io_unit_master > 0) THEN
         WRITE (io_unit_master, '(T7,A)') "(block index) @ process: (array index) value"
      END IF
      CALL dbcsr_t_iterator_start(iterator, tensor)
      DO WHILE (dbcsr_t_iterator_blocks_left(iterator))
         CALL dbcsr_t_iterator_next_block(iterator, blk_index, blk, blk_size=blk_size)
         CALL dbcsr_t_get_stored_coordinates(tensor, blk_index, proc)
         CALL mp_environ(numnodes, mynode, tensor%pgrid%mp_comm_2d)
         DBCSR_ASSERT(proc .EQ. mynode)
         #:for ndim in ndims
            IF (ndims_tensor(tensor) == ${ndim}$) THEN
               CALL dbcsr_t_get_block(tensor, blk_index, blk_values_${ndim}$, found)
               DBCSR_ASSERT(found)
               CALL dbcsr_t_write_block(tensor%name, blk_size, blk_index, proc, io_unit_all, &
                                        blk_values_${ndim}$=blk_values_${ndim}$, write_int=write_int)
               DEALLOCATE (blk_values_${ndim}$)
            END IF
         #:endfor
      END DO
      CALL dbcsr_t_iterator_stop(iterator)
   END SUBROUTINE

   SUBROUTINE dbcsr_t_write_block(name, blk_size, blk_index, proc, unit_nr, &
                                  ${varlist("blk_values",nmin=2)}$, write_int)
      !! Write a tensor block
      CHARACTER(LEN=*), INTENT(IN)                       :: name
         !! tensor name
      INTEGER, DIMENSION(:), INTENT(IN)                  :: blk_size
         !! block size
      INTEGER, DIMENSION(:), INTENT(IN)                  :: blk_index
         !! block index
      #:for ndim in ndims
         REAL(KIND=real_8), &
            DIMENSION(${arrlist("blk_size", nmax=ndim)}$), &
            INTENT(IN), OPTIONAL                            :: blk_values_${ndim}$
         !! block values for 2 dimensions
      #:endfor
      LOGICAL, INTENT(IN), OPTIONAL                      :: write_int
         !! write_int convert values to integers
      LOGICAL                                            :: write_int_prv
      INTEGER, INTENT(IN)                                :: unit_nr
         !! unit number
      INTEGER, INTENT(IN)                                :: proc
         !! which process am I
      INTEGER                                            :: ${varlist("i")}$
      INTEGER                                            :: ndim

      IF (PRESENT(write_int)) THEN
         write_int_prv = write_int
      ELSE
         write_int_prv = .FALSE.
      END IF

      ndim = SIZE(blk_size)

      IF (unit_nr > 0) THEN
         #:for ndim in ndims
            IF (ndim == ${ndim}$) THEN
               #:for idim in range(ndim,0,-1)
                  DO i_${idim}$ = 1, blk_size(${idim}$)
                     #:endfor
                     IF (write_int_prv) THEN
                        WRITE (unit_nr, '(T7,A,T16,A,${ndim}$I3,1X,A,1X,I3,A,1X,A,${ndim}$I3,1X,A,1X,I20)') &
                           TRIM(name), "(", blk_index, ") @", proc, ':', &
                           "(", ${varlist("i", nmax=ndim)}$, ")", &
                           INT(blk_values_${ndim}$ (${varlist("i", nmax=ndim)}$), KIND=int_8)
                     ELSE
                        WRITE (unit_nr, '(T7,A,T16,A,${ndim}$I3,1X,A,1X,I3,A,1X,A,${ndim}$I3,1X,A,1X,F10.5)') &
                           TRIM(name), "(", blk_index, ") @", proc, ':', &
                           "(", ${varlist("i", nmax=ndim)}$, ")", &
                           blk_values_${ndim}$ (${varlist("i", nmax=ndim)}$)
                     END IF
                     #:for idim in range(ndim,0,-1)
                        END DO
                     #:endfor
                  END IF
               #:endfor
            END IF
         END SUBROUTINE

         SUBROUTINE dbcsr_t_write_block_indices(tensor, io_unit_master, io_unit_all)
            TYPE(dbcsr_t_type), INTENT(INOUT)                  :: tensor
            INTEGER, INTENT(IN)                                :: io_unit_master, io_unit_all
            TYPE(dbcsr_t_iterator_type)                        :: iterator
            INTEGER, DIMENSION(ndims_tensor(tensor))          :: blk_index, blk_size
            INTEGER                                            :: blk, mynode, numnodes, proc

            IF (io_unit_master > 0) THEN
               WRITE (io_unit_master, '(T7,A)') "(block index) @ process: size"
            END IF

            CALL dbcsr_t_iterator_start(iterator, tensor)
            DO WHILE (dbcsr_t_iterator_blocks_left(iterator))
               CALL dbcsr_t_iterator_next_block(iterator, blk_index, blk, blk_size=blk_size)
               CALL dbcsr_t_get_stored_coordinates(tensor, blk_index, proc)
               CALL mp_environ(numnodes, mynode, tensor%pgrid%mp_comm_2d)
               DBCSR_ASSERT(proc .EQ. mynode)
               #:for ndim in ndims
                  IF (ndims_tensor(tensor) == ${ndim}$) THEN
                     WRITE (io_unit_all, '(T7,A,T16,A,${ndim}$I3,1X,A,1X,I3,A2,${ndim}$I3)') &
                        TRIM(tensor%name), "blk index (", blk_index, ") @", proc, ":", blk_size
                  END IF
               #:endfor
            END DO
            CALL dbcsr_t_iterator_stop(iterator)
         END SUBROUTINE

         SUBROUTINE dbcsr_t_write_split_info(pgrid, unit_nr)
            TYPE(dbcsr_t_pgrid_type), INTENT(IN) :: pgrid
            INTEGER, INTENT(IN) :: unit_nr

            IF (ALLOCATED(pgrid%tas_split_info)) THEN
               CALL dbcsr_tas_write_split_info(pgrid%tas_split_info, unit_nr)
            END IF
         END SUBROUTINE

         FUNCTION prep_output_unit(unit_nr) RESULT(unit_nr_out)
            INTEGER, INTENT(IN), OPTIONAL :: unit_nr
            INTEGER                       :: unit_nr_out

            IF (PRESENT(unit_nr)) THEN
               unit_nr_out = unit_nr
            ELSE
               unit_nr_out = 0
            END IF

         END FUNCTION
      END MODULE
