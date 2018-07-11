//------------------------------------------------------------------------------
// Copyright (c) 2017, University of Tennessee
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of the University of Tennessee nor the
//       names of its contributors may be used to endorse or promote products
//       derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL UNIVERSITY OF TENNESSEE BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//------------------------------------------------------------------------------
// This research was supported by the Exascale Computing Project (17-SC-20-SC),
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.
//------------------------------------------------------------------------------
// Need assistance with the SLATE software? Join the "SLATE User" Google group
// by going to https://groups.google.com/a/icl.utk.edu/forum/#!forum/slate-user
// and clicking "Apply to join group". Upon acceptance, email your questions and
// comments to <slate-user@icl.utk.edu>.
//------------------------------------------------------------------------------

#include "slate.hh"
#include "slate_internal.hh"
#include "slate_internal_util.hh"
#include "slate_mpi.hh"

#include <list>
#include <tuple>

namespace slate {

// specialization namespace differentiates, e.g.,
// internal::trnorm from internal::specialization::trnorm
namespace internal {
namespace specialization {

//------------------------------------------------------------------------------
/// @internal
/// Distributed parallel trapezoid and triangular matrix norm.
/// Generic implementation for any target.
/// @ingroup trnorm
template <Target target, typename scalar_t>
blas::real_type<scalar_t>
trnorm(slate::internal::TargetType<target>,
       Norm norm, Diag diag, TrapezoidMatrix<scalar_t>& A)
{
    using real_t = blas::real_type<scalar_t>;

    //---------
    // max norm
    // max_{i,j} abs( A_{i,j} )
    if (norm == Norm::Max) {
        real_t local_max;
        real_t global_max;

        if (target == Target::Devices)
            A.reserveDeviceWorkspace();

        #pragma omp parallel
        #pragma omp master
        {
            internal::trnorm<target>(norm, diag, std::move(A), &local_max);
        }

        MPI_Op op_max_nan;
        #pragma omp critical(slate_mpi)
        {
            slate_mpi_call(
                MPI_Op_create(mpi_max_nan, true, &op_max_nan));
        }

        #pragma omp critical(slate_mpi)
        {
            trace::Block trace_block("MPI_Allreduce");
            slate_mpi_call(
                MPI_Allreduce(&local_max, &global_max,
                              1, mpi_type<real_t>::value,
                              op_max_nan, A.mpiComm()));
        }

        #pragma omp critical(slate_mpi)
        {
            slate_mpi_call(
                MPI_Op_free(&op_max_nan));
        }

        A.clearWorkspace();

        return global_max;
    }
    //---------
    // one norm
    // max col sum = max_j sum_i abs( A_{i,j} )
    else if (norm == Norm::One) {
        std::vector<real_t> local_sums(A.n());

        if (target == Target::Devices)
            A.reserveDeviceWorkspace();

        #pragma omp parallel
        #pragma omp master
        {
            internal::trnorm<target>(norm, diag, std::move(A), local_sums.data());
        }

        std::vector<real_t> global_sums(A.n());

        #pragma omp critical(slate_mpi)
        {
            trace::Block trace_block("MPI_Allreduce");
            slate_mpi_call(
                MPI_Allreduce(local_sums.data(), global_sums.data(),
                              A.n(), mpi_type<real_t>::value,
                              MPI_SUM, A.mpiComm()));
        }

        A.clearWorkspace();

        return lapack::lange(Norm::Max, 1, A.n(), global_sums.data(), 1);
    }
    //---------
    // inf norm
    // max row sum = max_i sum_j abs( A_{i,j} )
    else if (norm == Norm::Inf) {
        std::vector<real_t> local_sums(A.m());

        if (target == Target::Devices)
            A.reserveDeviceWorkspace();

        #pragma omp parallel
        #pragma omp master
        {
            internal::trnorm<target>(norm, diag, std::move(A), local_sums.data());
        }

        std::vector<real_t> global_sums(A.m());

        #pragma omp critical(slate_mpi)
        {
            trace::Block trace_block("MPI_Allreduce");
            slate_mpi_call(
                MPI_Allreduce(local_sums.data(), global_sums.data(),
                              A.m(), mpi_type<real_t>::value,
                              MPI_SUM, A.mpiComm()));
        }

        A.clearWorkspace();

        return lapack::lange(Norm::Max, 1, A.m(), global_sums.data(), 1);
    }
    //---------
    // Frobenius norm
    // sqrt( sum_{i,j} abs( A_{i,j} )^2 )
    else if (norm == Norm::Fro) {
        real_t local_values[2];
        real_t local_sumsq;
        real_t global_sumsq;

        if (target == Target::Devices)
            A.reserveDeviceWorkspace();

        #pragma omp parallel
        #pragma omp master
        {
            internal::trnorm<target>(norm, diag, std::move(A), local_values);
        }

        #pragma omp critical(slate_mpi)
        {
            trace::Block trace_block("MPI_Allreduce");
            // todo: propogate scale
            local_sumsq = local_values[0] * local_values[0] * local_values[1];
            slate_mpi_call(
                MPI_Allreduce(&local_sumsq, &global_sumsq,
                              1, mpi_type<real_t>::value,
                              MPI_SUM, A.mpiComm()));
        }

        A.clearWorkspace();

        return sqrt(global_sumsq);
    }
    else {
        throw std::exception();  // todo: invalid norm
    }
}

} // namespace specialization
} // namespace internal

//------------------------------------------------------------------------------
/// Version with target as template parameter.
/// @ingroup trnorm
template <Target target, typename scalar_t>
blas::real_type<scalar_t>
trnorm(Norm norm, Diag diag, TrapezoidMatrix<scalar_t>& A,
       const std::map<Option, Value>& opts)
{
    return internal::specialization::trnorm(internal::TargetType<target>(),
                                            norm, diag, A);
}

//------------------------------------------------------------------------------
/// Distributed parallel trapezoid and triangular matrix norm.
///
//------------------------------------------------------------------------------
/// @tparam scalar_t
///     One of float, double, std::complex<float>, std::complex<double>.
//------------------------------------------------------------------------------
/// @param[in] norm
///     Norm to compute:
///     - Norm::Max: maximum element,    $\max_{i, j}   \abs( A_{i, j} )$
///     - Norm::One: maximum column sum, $\max_j \sum_i \abs( A_{i, j} )$
///     - Norm::Inf: maximum row sum,    $\max_i \sum_j \abs( A_{i, j} )$
///     - Norm::Fro: Frobenius norm, $\sqrt( \sum_{i, j} \abs( A_{i, j} )^2 )$
///
/// @param[in] diag
///     Whether or not A is unit triangular:
///     - Diag::NonUnit: A is non-unit triangular;
///     - Diag::Unit:    A is unit triangular.
///                      The diagonal elements of A are not referenced
///                      and are assumed to be 1.
///
/// @param[in] A
///     The m-by-n trapezoid matrix A.
///
/// @param[in] opts
///     Additional options, as map of name = value pairs. Possible options:
///     - Option::Target:
///       Implementation to target. Possible values:
///       - HostTask:  OpenMP tasks on CPU host [default].
///       - HostNest:  nested OpenMP parallel for loop on CPU host.
///       - Devices:   batched BLAS on GPU device.
///
/// @ingroup trnorm
template <typename scalar_t>
blas::real_type<scalar_t>
trnorm(Norm norm, Diag diag, TrapezoidMatrix<scalar_t>& A,
       const std::map<Option, Value>& opts)
{
    Target target;
    try {
        target = Target(opts.at(Option::Target).i_);
    }
    catch (std::out_of_range) {
        target = Target::HostTask;
    }

    switch (target) {
        case Target::Host:
        case Target::HostTask:
            return trnorm<Target::HostTask>(norm, diag, A, opts);
            break;
        case Target::HostBatch:
        case Target::HostNest:
            return trnorm<Target::HostNest>(norm, diag, A, opts);
            break;
        case Target::Devices:
            return trnorm<Target::Devices>(norm, diag, A, opts);
            break;
    }
    throw std::exception();  // todo: invalid target
}

//------------------------------------------------------------------------------
// Explicit instantiations.
template
float trnorm<float>(
    Norm norm, Diag diag, TrapezoidMatrix<float>& A,
    const std::map<Option, Value>& opts);

template
double trnorm<double>(
    Norm norm, Diag diag, TrapezoidMatrix<double>& A,
    const std::map<Option, Value>& opts);

template
float trnorm< std::complex<float> >(
    Norm norm, Diag diag, TrapezoidMatrix< std::complex<float> >& A,
    const std::map<Option, Value>& opts);

template
double trnorm< std::complex<double> >(
    Norm norm, Diag diag, TrapezoidMatrix< std::complex<double> >& A,
    const std::map<Option, Value>& opts);

} // namespace slate