 /*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "conic_activeset.hpp"
#include "casadi/core/nlpsol.hpp"

using namespace std;
namespace casadi {

  extern "C"
  int CASADI_CONIC_ACTIVESET_EXPORT
  casadi_register_conic_activeset(Conic::Plugin* plugin) {
    plugin->creator = ConicActiveSet::creator;
    plugin->name = "activeset";
    plugin->doc = ConicActiveSet::meta_doc.c_str();
    plugin->version = CASADI_VERSION;
    plugin->options = &ConicActiveSet::options_;
    return 0;
  }

  extern "C"
  void CASADI_CONIC_ACTIVESET_EXPORT casadi_load_conic_activeset() {
    Conic::registerPlugin(casadi_register_conic_activeset);
  }

  ConicActiveSet::ConicActiveSet(const std::string& name, const std::map<std::string, Sparsity> &st)
    : Conic(name, st) {
  }

  ConicActiveSet::~ConicActiveSet() {
    clear_mem();
  }

  Options ConicActiveSet::options_
  = {{&Conic::options_},
     {{"max_iter",
       {OT_INT,
        "Maximum number of iterations [1000]."}},
      {"tol",
       {OT_DOUBLE,
        "Tolerance [1e-8]."}}
     }
  };

  void ConicActiveSet::init(const Dict& opts) {
    // Initialize the base classes
    Conic::init(opts);

    // Default options
    max_iter_ = 1000;
    tol_ = 1e-8;

    // Read user options
    for (auto&& op : opts) {
      if (op.first=="max_iter") {
        max_iter_ = op.second;
      } else if (op.first=="tol") {
        tol_ = op.second;
      }
    }

    // Assemble KKT system sparsity
    kkt_ = Sparsity::kkt(H_, A_, false);

    // Transpose of the Jacobian
    AT_ = A_.T();

    // KKT with diagonal
    kktd_ = kkt_ + Sparsity::diag(nx_ + na_);

    // Symbolic QR factorization
    kktd_.qr_sparse(sp_v_, sp_r_, prinv_, pc_);

    // Allocate memory
    alloc_w(kkt_.nnz(), true); // kkt
    alloc_w(kktd_.nnz(), true); // kktd
    alloc_w(nx_+na_, true); // z=[xk,gk]
    alloc_w(nx_+na_, true); // lbz
    alloc_w(nx_+na_, true); // ubz
    alloc_w(nx_+na_, true); // lam
    alloc_w(AT_.nnz(), true); // trans_a
    alloc_iw(nx_+na_); // casadi_trans, tau type
    alloc_w(nx_+na_); // casadi_project, tau memory
    alloc_w(nx_+na_, true); // dz
    alloc_w(nx_+na_, true); // dlam
    alloc_w(nx_, true); // glag
    alloc_w(nx_, true); // infeas
    alloc_w(nx_, true); // tinfeas
    alloc_iw(nx_+na_, true); // neverzero
    alloc_iw(nx_+na_, true); // neverupper
    alloc_iw(nx_+na_, true); // neverlower
    alloc_iw(nx_+na_); // allzero
    alloc_iw(nx_+na_, true); // flipme

    // Memory for numerical solution
    alloc_w(max(sp_v_.nnz()+sp_r_.nnz(), kktd_.nnz()), true); // either v & r or trans(kktd)
    alloc_w(nx_+na_, true); // beta
    alloc_w(2*na_+2*nx_); // casadi_qr

    // Print summary
    print("-------------------------------------------\n");
    print("This is casadi::ConicActiveSet.\n");
    print("Number of variables:                       %9d\n", nx_);
    print("Number of constraints:                     %9d\n", na_);
    print("Work in progress!\n");
  }

  template<typename T1>
  void casadi_set_sub(const T1* y, T1* x, const casadi_int* sp_x,
                      casadi_int rbeg, casadi_int rend,
                      casadi_int cbeg, casadi_int cend) {
    // Local variables
    casadi_int r, c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2, *row=sp_x+2+ncol+1;
    // Set elements in subblock
    for (c=cbeg; c<cend; ++c) {
      for (k=colind[c]; k<colind[c+1] && (r=row[k])<rend; ++k) {
        if (r>=rbeg) x[k] = *y++;
      }
    }
  }

  template<typename T1>
  void casadi_fill_sub(T1 y, T1* x, const casadi_int* sp_x,
                      casadi_int rbeg, casadi_int rend,
                      casadi_int cbeg, casadi_int cend) {
    // Local variables
    casadi_int r, c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2, *row=sp_x+2+ncol+1;
    // Set elements in subblock
    for (c=cbeg; c<cend; ++c) {
      for (k=colind[c]; k<colind[c+1] && (r=row[k])<rend; ++k) {
        if (r>=rbeg) x[k] = y;
      }
    }
  }

  template<typename T1>
  void casadi_row_scal(T1* x, const casadi_int* sp_x, const T1* d) {
    // Local variables
    casadi_int c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2, *row=sp_x+2+ncol+1;
    // Scale entries
    for (c=0; c<ncol; ++c) {
      for (k=colind[c]; k<colind[c+1]; ++k) {
        x[k] *= d[row[k]];
      }
    }
  }

  void ConicActiveSet::
  print_vector(const char* id, const double* x, casadi_int n) const {
    print("%s: [", id);
    for (casadi_int i=0; i<n; ++i) {
      if (i!=0) print(", ");
      print("%g", x[i]);
    }
    print("]\n");
  }

  void ConicActiveSet::
  print_ivector(const char* id, const casadi_int* x, casadi_int n) const {
    print("%s: [", id);
    for (casadi_int i=0; i<n; ++i) {
      if (i!=0) print(", ");
      print("%lld", x[i]);
    }
    print("]\n");
  }

  void ConicActiveSet::
  print_signs(const char* id, const double* x, casadi_int n) const {
    print("%s: [", id);
    for (casadi_int i=0; i<n; ++i) {
      print(x[i]==0 ? "0" : x[i]>0 ? "+" : "-");
    }
    print("]\n");
  }

  void print_matrix(const char* id, const double* x, const casadi_int* sp_x) {
    cout << id << ": ";
    Sparsity sp = Sparsity::compressed(sp_x);
    vector<double> nz(sp.nnz(), 0.);
    if (x!=0) casadi_copy(x, nz.size(), get_ptr(nz));
    DM(sp, nz).print_dense(cout, false);
    cout << endl;
  }

  template<typename T1>
  void casadi_col_scal(T1* x, const casadi_int* sp_x, const T1* d) {
    // Local variables
    casadi_int c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2;
    // Scale entries
    for (c=0; c<ncol; ++c) {
      for (k=colind[c]; k<colind[c+1]; ++k) {
        x[k] *= d[c];
      }
    }
  }

  template<typename T1>
  void casadi_add_diag(T1* x, const casadi_int* sp_x, const T1* d) {
    // Local variables
    casadi_int c, k;
    // Get sparsities
    casadi_int ncol=sp_x[1];
    const casadi_int *colind=sp_x+2, *row=sp_x+2+ncol+1;
    // Add to diagonal entry
    for (c=0; c<ncol; ++c) {
      for (k=colind[c]; k<colind[c+1]; ++k) {
        if (row[k]==c) {
          x[k] += d[c];
          break;
        }
      }
    }
  }

  int ConicActiveSet::init_mem(void* mem) const {
    //auto m = static_cast<ConicActiveSetMemory*>(mem);
    return 0;
  }

  int ConicActiveSet::
  eval(const double** arg, double** res, casadi_int* iw, double* w, void* mem) const {
    auto m = static_cast<ConicActiveSetMemory*>(mem);

    // Statistics
    for (auto&& s : m->fstats) s.second.reset();

    if (inputs_check_) {
      check_inputs(arg[CONIC_LBX], arg[CONIC_UBX], arg[CONIC_LBA], arg[CONIC_UBA]);
    }

    // Local variables
    casadi_int i, k, flag;
    double fk;
    // Get input pointers
    const double *h, *g, *a, *lba, *uba, *lbx, *ubx, *x0, *lam_x0, *lam_a0;
    h = arg[CONIC_H];
    g = arg[CONIC_G];
    a = arg[CONIC_A];
    lba = arg[CONIC_LBA];
    uba = arg[CONIC_UBA];
    lbx = arg[CONIC_LBX];
    ubx = arg[CONIC_UBX];
    x0 = arg[CONIC_X0];
    lam_x0 = arg[CONIC_LAM_X0];
    lam_a0 = arg[CONIC_LAM_A0];

    // Get output pointers
    double *x, *f, *lam_a, *lam_x;
    x = res[CONIC_X];
    f = res[CONIC_COST];
    lam_a = res[CONIC_LAM_A];
    lam_x = res[CONIC_LAM_X];

    // Work vectors
    double *kkt, *kktd, *z, *lam, *v, *r, *beta, *dz, *dlam, *lbz, *ubz,
           *glag, *trans_a, *infeas, *tinfeas, *vr;
    kkt = w; w += kkt_.nnz();
    kktd = w; w += kktd_.nnz();
    z = w; w += nx_+na_;
    lbz = w; w += nx_+na_;
    ubz = w; w += nx_+na_;
    lam = w; w += nx_+na_;
    dz = w; w += nx_+na_;
    dlam = w; w += nx_+na_;
    vr = w; w += max(sp_v_.nnz()+sp_r_.nnz(), kktd_.nnz());
    v = vr;
    r = vr + sp_v_.nnz();
    beta = w; w += nx_+na_;
    glag = w; w += nx_;
    trans_a = w; w += AT_.nnz();
    infeas = w; w += nx_;
    tinfeas = w; w += nx_;
    casadi_int *neverzero, *neverupper, *neverlower, *flipme;
    neverzero = iw; iw += nx_+na_;
    neverupper = iw; iw += nx_+na_;
    neverlower = iw; iw += nx_+na_;
    flipme = iw; iw += nx_+na_;

    // Smallest strictly positive number
    const double DMIN = std::numeric_limits<double>::min();

    // Bounds on z
    casadi_copy(lbx, nx_, lbz);
    casadi_copy(lba, na_, lbz+nx_);
    casadi_copy(ubx, nx_, ubz);
    casadi_copy(uba, na_, ubz+nx_);

    if (verbose_) {
      print_vector("lbz", lbz, nx_+na_);
      print_vector("ubz", ubz, nx_+na_);
      print_matrix("H", h, H_);
      print_matrix("A", a, A_);
    }

    // Pass initial guess
    casadi_copy(x0, nx_, z);
    casadi_copy(lam_x0, nx_, lam);
    casadi_copy(lam_a0, na_, lam+nx_);

    // Transpose A
    casadi_trans(a, A_, trans_a, AT_, iw);

    // Assemble the KKT matrix
    casadi_set_sub(h, kkt, kkt_, 0, nx_, 0, nx_); // h
    casadi_set_sub(a, kkt, kkt_, nx_, nx_+na_, 0, nx_); // a
    casadi_set_sub(trans_a, kkt, kkt_, 0, nx_, nx_, nx_+na_); // a'

    // Look for all-zero rows in kkt
    const casadi_int* kkt_colind = kkt_.colind();
    const casadi_int* kkt_row = kkt_.row();
    for (casadi_int c=0; c<nx_+na_; ++c) iw[c] = 1;
    for (casadi_int c=0; c<nx_+na_; ++c) {
      for (casadi_int k=kkt_colind[c]; k<kkt_colind[c+1]; ++k) {
        if (fabs(kkt[k])>1e-16) iw[kkt_row[k]] = 0;
      }
    }

    // Permitted signs for lam
    for (casadi_int c=0; c<nx_+na_; ++c) {
      neverzero[c] = lbz[c]==ubz[c];
      neverupper[c] = isinf(ubz[c]);
      neverlower[c] = isinf(lbz[c]);
      if (iw[c]) {
        // All-zero row
        if (c<nx_) {
          // Inactive constraint would lead to singular KKT
          neverzero[c] = 1;
        } else {
          // Active constraint would lead to singular KKT
          neverupper[c] = neverlower[c] = 1;
        }
      }
    }

    // Calculate g
    casadi_fill(z+nx_, na_, 0.);
    casadi_mv(a, A_, z, z+nx_, 0);

    // Determine initial active set
    for (i=0; i<nx_+na_; ++i) {
      casadi_assert(!neverzero[i] || !neverupper[i] || !neverlower[i],
                    "No sign possible for " + str(i));
      if (!neverzero[i]) {
        // All inequality constraints are inactive
        lam[i] = 0;
      } else if (neverupper[i] || z[i]<=lbz[i]) {
        // Lower bound active (including satisfied bounds)
        lam[i] = fmin(lam[i], -DMIN);
      } else {
        // Upper bound active (excluding satisfied bounds)
        lam[i] = fmax(lam[i],  DMIN);
      }
    }

    // kktd sparsity
    const casadi_int* kktd_colind = kktd_.colind();
    const casadi_int* kktd_row = kktd_.row();

    // R sparsity
    //const casadi_int* r_colind = sp_r_.colind();
    //const casadi_int* r_row = sp_r_.row();

    // A sparsity
    //const casadi_int* a_colind = A_.colind();
    //const casadi_int* a_row = A_.row();

    // AT sparsity
    const casadi_int* at_colind = AT_.colind();
    const casadi_int* at_row = AT_.row();

    // Message buffer
    char msg[40] = "";

    // No change so far
    bool new_active_set = true;

    // Stepsize
    double tau = 0.;

    // Smallest diagonal value for the QR factorization
    double mina = -1;
    casadi_int imina = -1;

    // Singularity in the last iteration
    casadi_int sing, sing_ind, sing_sign;

    // QP iterations
    casadi_int iter = 0;
    while (true) {
      // Debugging
      if (verbose_) {
        print_vector("z", z, nx_+na_);
        print_vector("lam", lam, nx_+na_);
        print_signs("sign(lam)", lam, nx_+na_);
      }

      // Recalculate g
      casadi_fill(z+nx_, na_, 0.);
      casadi_mv(a, A_, z, z+nx_, 0);

      // Evaluate gradient of the Lagrangian and constraint functions
      casadi_copy(g, nx_, glag);
      casadi_mv(h, H_, z, glag, 0); // gradient of the objective
      casadi_mv(a, A_, lam+nx_, glag, 1); // gradient of the Lagrangian

      // Recalculate lam(x), without changing the sign
      for (i=0; i<nx_; ++i) {
        if (lam[i]>0) {
          lam[i] = fmax(-glag[i], DMIN);
        } else if (lam[i]<0) {
          lam[i] = fmin(-glag[i], -DMIN);
        }
      }

      // Calculate cost
      fk = casadi_bilin(h, H_, z, z)/2. + casadi_dot(nx_, z, g);

      // Look for largest bound violation
      double prerr = 0.;
      casadi_int iprerr = -1;
      bool prerr_pos;
      for (i=0; i<nx_+na_; ++i) {
        if (z[i] > ubz[i]+prerr) {
          prerr = z[i]-ubz[i];
          iprerr = i;
          prerr_pos = true;
        } else if (z[i] < lbz[i]-prerr) {
          prerr = lbz[i]-z[i];
          iprerr = i;
          prerr_pos = false;
        }
      }

      // Calculate dual infeasibility
      double duerr = 0.;
      casadi_int iduerr = -1;
      bool duerr_pos;
      for (i=0; i<nx_; ++i) {
        infeas[i] = glag[i]+lam[i];
        double duerr_trial = fabs(infeas[i]);
        if (duerr_trial>duerr) {
          duerr = duerr_trial;
          iduerr = i;
          duerr_pos = glag[i]+lam[i]>0;
        }
      }

      // If last step was full, add constraint?
      if (!new_active_set) {
        if (sing) {
          casadi_assert_dev(sing_ind>=0);
          i = sing_ind;
          print("Flip %lld? i=%lld, z=%g, lbz=%g, ubz=%g, lam=%g, dz=%g, dlam=%g, tau=%g\n",
                sing_sign, i, z[i], lbz[i], ubz[i], lam[i], dz[i], dlam[i], tau);
          lam[sing_ind] = sing_sign==0 ? 0. : sing_sign<0 ? -DMIN : DMIN;
          new_active_set = true;
          sprint(msg, sizeof(msg), "sign(lam[%lld])=%lld", sing_ind, sing_sign);
        } else if (iprerr>=0 && lam[iprerr]==0.) {
          // Try to improve primal feasibility
          lam[iprerr] = z[iprerr]<lbz[iprerr] ? -DMIN : DMIN;
          new_active_set = true;
          sprint(msg, sizeof(msg), "Added %lld to reduce |pr|", iprerr);
        }
      }

      // Copy kkt to kktd
      casadi_project(kkt, kkt_, kktd, kktd_, w);

      // Loop over kktd entries (left two blocks of the transposed KKT)
      for (casadi_int c=0; c<nx_; ++c) {
        if (lam[c]!=0) {
          // Zero out column, set diagonal entry to 1
          for (k=kktd_colind[c]; k<kktd_colind[c+1]; ++k) {
            kktd[k] = kktd_row[k]==c ? 1. : 0.;
          }
        }
      }

      // Loop over kktd entries (right two blocks of the transposed KKT)
      for (casadi_int c=0; c<na_; ++c) {
        if (lam[nx_+c]==0) {
          // Zero out column, set diagonal entry to -1
          for (k=kktd_colind[nx_+c]; k<kktd_colind[nx_+c+1]; ++k) {
            kktd[k] = kktd_row[k]==nx_+c ? -1. : 0.;
          }
        }
      }

      if (verbose_) {
        print_matrix("KKT", kktd, kktd_);
      }

      // QR factorization
      casadi_qr(kktd_, kktd, w, sp_v_, v, sp_r_, r, beta, get_ptr(prinv_), get_ptr(pc_));
      if (verbose_) {
        print_matrix("QR(R)", r, sp_r_);
      }
      // Check singularity
      sing = casadi_qr_singular(&mina, &imina, r, sp_r_, get_ptr(pc_), 1e-12);

      if (iter % 10 == 0) {
        // Print header
        print("%10s %15s %15s %6s %15s %6s %15s %6s %10s %40s\n",
              "Iteration", "fk", "|pr|", "con", "|du|", "var",
              "min(diag(R))", "con", "last tau", "Note");
      }

      // Print iteration progress:
      print("%6d (%1s) %15g %15g %6d %15g %6d %15g %6d %10g %40s\n",
            iter, sing ? "S" : "F", fk, prerr, iprerr, duerr, iduerr,
            mina, imina, tau, msg);

      // Successful return if still no change
      if (!new_active_set) {
        flag = 0;
        break;
      }

      // Too many iterations?
      if (iter>=max_iter_) {
        casadi_warning("Maximum number of iterations reached");
        flag = 1;
        break;
      }

      // Start new iteration
      iter++;
      msg[0] = '\0';

      // No change so far
      new_active_set = false;

      // Calculate search direction
      if (sing) {
        // Get a linear combination of the columns in kktd
        casadi_qr_colcomb(dz, r, sp_r_, get_ptr(pc_), imina);
      } else {
        // KKT residual
        for (i=0; i<nx_+na_; ++i) {
          if (lam[i]>0.) {
            dz[i] = z[i]-ubz[i];
          } else if (lam[i]<0.) {
            dz[i] = z[i]-lbz[i];
          } else if (i<nx_) {
            dz[i] = glag[i];
          } else {
            dz[i] = -lam[i];
          }
        }

        // Solve to get primal-dual step
        casadi_scal(nx_+na_, -1., dz);
        casadi_qr_solve(dz, 1, 1, sp_v_, v, sp_r_, r, beta,
                        get_ptr(prinv_), get_ptr(pc_), w);
      }

      // Calculate change in Lagrangian gradient
      casadi_fill(dlam, nx_, 0.);
      casadi_mv(h, H_, dz, dlam, 0); // gradient of the objective
      casadi_mv(a, A_, dz+nx_, dlam, 1); // gradient of the Lagrangian

      // Step in lam(x)
      casadi_scal(nx_, -1., dlam);

      // For inactive constraints, lam(x) step is zero
      for (i=0; i<nx_; ++i) if (lam[i]==0.) dlam[i] = 0.;

      // Step in lam(g)
      casadi_copy(dz+nx_, na_, dlam+nx_);

      // Step in z(g)
      casadi_fill(dz+nx_, na_, 0.);
      casadi_mv(a, A_, dz, dz+nx_, 0);

      // Print search direction
      if (verbose_) {
        print_vector("dz", dz, nx_+na_);
        print_vector("dlam", dlam, nx_+na_);
      }

      // Tangent of the dual infeasibility at tau=0
      casadi_fill(tinfeas, nx_, 0.);
      casadi_mv(h, H_, dz, tinfeas, 0); // A'*dlam_g + dlam_x==0 by definition
      casadi_mv(a, A_, dlam+nx_, tinfeas, 1);
      casadi_axpy(nx_, 1., dlam, tinfeas);

      // Handle singularity
      if (sing) {
        // Change in err in the search direction
        double prtau = iprerr<0 ? 0. : prerr_pos ? dz[iprerr]/prerr : -dz[iprerr]/prerr;
        double dutau = iduerr<0 ? 0. : tinfeas[iduerr]/infeas[iduerr];
        double derr = prerr>=duerr ? prtau : dutau;

        // QR factorization of the transpose
        casadi_trans(kktd, kktd_, vr, kktd_, iw);
        casadi_copy(vr, kktd_.nnz(), kktd);
        casadi_qr(kktd_, kktd, w, sp_v_, v, sp_r_, r, beta, get_ptr(prinv_), get_ptr(pc_));
        // Get a linear combination of the rows in kktd
        double minat_tr;
        casadi_int imina_tr;
        casadi_qr_singular(&minat_tr, &imina_tr, r, sp_r_, get_ptr(pc_), 1e-12);
        casadi_qr_colcomb(w, r, sp_r_, get_ptr(pc_), imina_tr);
        if (verbose_) {
          print_vector("normal", w, nx_+na_);
        }
        // Best flip
        double tau_test, best_tau = inf;
        sing_ind = -1;
        // Which constraints can be flipped in order to restore regularity?
        casadi_int nflip = 0;
        for (i=0; i<nx_+na_; ++i) {
          flipme[i] = 0;
          // Check if old column can be removed without decreasing rank
          if (fabs(i<nx_ ? dz[i] : dlam[i])<1e-12) continue;
          // If dot(w, kktd(:,i)-kktd_flipped(:,i))==0, rank won't increase
          double d = i<nx_ ? w[i] : -w[i];
          for (k=kkt_colind[i]; k<kkt_colind[i+1]; ++k) d -= kkt[k]*w[kkt_row[k]];
          if (fabs(d)<1e-12) continue;
          // When at the bound, ensure that flipping won't increase dual error
          if (lam[i]!=0.) {
            bool at_bound=false, increasing;
            if (i<nx_) {
              // Box constraints
              if (duerr==fabs(glag[i])) {
                at_bound = true;
                increasing = (glag[i]>0.) != (lam[i]>0.);
              }
            } else {
              // Linear constraints, check all
              for (k=at_colind[i-nx_]; k<at_colind[i-nx_+1]; ++k) {
                casadi_int j = at_row[k];
                if (duerr==fabs(infeas[j]-trans_a[k]*lam[i])) {
                  at_bound = true;
                  increasing = trans_a[k]!=0.
                      && (infeas[j]>0.) != ((trans_a[k]>0.) == (lam[i]>0.));
                  if (increasing) break;
                }
              }
            }
            // We're at the bound and setting lam[i]=0 would increase error
            if (at_bound && increasing) continue;
          }
          // Is constraint active?
          if (lam[i]==0.) {
            // Make sure that step is nonzero
            if (fabs(dz[i])<1e-12) continue;
            // Step needed to bring z to lower bound
            if (!neverlower[i]) {
              tau_test = (lbz[i]-z[i])/dz[i];
              // Ensure nonincrease in max(prerr, duerr)
              if (!((derr>0. && tau_test>0.) || (derr<0. && tau_test<0.))) {
                // Only allow removing constraints if tau_test==0
                if (fabs(tau_test)>=1e-16) {
                  // Check if best so far
                  if (fabs(tau_test)<fabs(best_tau)) {
                    best_tau = tau_test;
                    sing_ind = i;
                    sing_sign = -1;
                  }
                }
              }
            }
            // Step needed to bring z to upper bound
            if (!neverupper[i]) {
              tau_test = (ubz[i]-z[i])/dz[i];
              // Ensure nonincrease in max(prerr, duerr)
              if (!((derr>0. && tau_test>0.) || (derr<0. && tau_test<0.))) {
                // Only allow removing constraints if tau_test==0
                if (fabs(tau_test)>=1e-16) {
                  // Check if best so far
                  if (fabs(tau_test)<fabs(best_tau)) {
                    best_tau = tau_test;
                    sing_ind = i;
                    sing_sign = 1;
                  }
                }
              }
            }
          } else {
            // Make sure that step is nonzero
            if (fabs(dlam[i])<1e-12) continue;
            // Step needed to bring lam to zero
            if (!neverzero[i]) {
              tau_test = -lam[i]/dlam[i];
              // Ensure nonincrease in max(prerr, duerr)
              if ((derr>0. && tau_test>0.) || (derr<0. && tau_test<0.)) continue;
              // Check if best so far
              if (fabs(tau_test)<fabs(best_tau)) {
                best_tau = tau_test;
                sing_ind = i;
                sing_sign = 0;
              }
            }
          }
          flipme[i] = 1;
          nflip++;
        }

        if (sing_ind>=0) {
          if (fabs(best_tau)<1e-12) {
            // Zero step
            tau = 0.;
            continue;
          }
        } else {
          casadi_warning("Cannot restore feasibility");
          flag = 1;
          break;
        }

        if (nflip==0) {
          casadi_warning("Cannot restore feasibility");
          flag = 1;
          break;
        }

        if (verbose_) {
          print_ivector("flippable", flipme, nx_+na_);
        }

        // Scale step so that tau=1 is full step
        casadi_scal(nx_+na_, best_tau, dz);
        casadi_scal(nx_+na_, best_tau, dlam);
        casadi_scal(nx_, best_tau, tinfeas);
      }

      // Get maximum step size and corresponding index and new sign
      tau = 1.;
      casadi_int sign=0, index=-1;

      // Check if the step is nonzero
      bool zero_step = true;
      for (i=0; i<nx_+na_ && zero_step; ++i) zero_step = dz[i]==0.;
      for (i=0; i<nx_+na_ && zero_step; ++i) zero_step = dlam[i]==0.;
      if (zero_step) tau = 0.;

      // Warning if step becomes zero
      if (zero_step) casadi_warning("No search direction");

      // Check primal feasibility in the search direction
      for (i=0; i<nx_+na_ && tau>0.; ++i) {
        double tau1 = tau;
        // Acceptable primal error (must be non-increasing)
        double e = fmax(prerr, 1e-10);
        if (dz[i]==0.) continue; // Skip zero steps
        // Check if violation with tau=0 and not improving
        if (dz[i]<0 ? z[i]<=lbz[i]-e : z[i]>=ubz[i]+e) {
          tau = 0.;
          index = i;
          sign = dz[i]<0 ? -1 : 1;
          break;
        }
        // Trial primal step
        double trial_z=z[i] + tau*dz[i];
        if (dz[i]<0 && trial_z<lbz[i]-e) {
          // Trial would increase maximum infeasibility
          tau = (lbz[i]-e-z[i])/dz[i];
          index = i;
          sign = -1;
        } else if (dz[i]>0 && trial_z>ubz[i]+e) {
          // Trial would increase maximum infeasibility
          tau = (ubz[i]+e-z[i])/dz[i];
          index = i;
          sign = 1;
        }
        // Consistency check
        casadi_assert(tau<=tau1, "Inconsistent step size calculation");
      }

      // Calculate and order all tau for which there is a sign change
      casadi_fill(w, nx_+na_, 1.);
      casadi_int n_tau = 0;
      for (i=0; i<nx_+na_; ++i) {
        if (dlam[i]==0.) continue; // Skip zero steps
        if (lam[i]==0.) continue; // Skip inactive constraints
        // Skip full steps
        if (lam[i]>0 ? lam[i]>=-dlam[i] : lam[i]<=-dlam[i]) continue;
        // Trial dual step
        double trial_lam = lam[i] + tau*dlam[i];
        if (lam[i]>0 ? trial_lam < -0. : trial_lam > 0.) {
          w[i] = -lam[i]/dlam[i];
        }
        // Where to insert the w[i]
        casadi_int loc;
        for (loc=0; loc<n_tau; ++loc) {
          if (w[i]<w[iw[loc]]) break;
        }
        // Insert element
        n_tau++;
        casadi_int next=i, tmp;
        for (casadi_int j=loc; j<n_tau; ++j) {
          tmp = iw[j];
          iw[j] = next;
          next = tmp;
        }
      }
      // Acceptable dual error (must be non-increasing)
      double e = fmax(duerr, 1e-10);
      /* With the search direction (dz, dlam) and the restriction that when
         lam=0, it stays at zero, we have the following expressions for the
         updated step in the presence of a zero-crossing
             z(tau)   = z(0) + tau*dz
             lam(tau) = lam(0) + tau*dlam     if tau<=tau1
                        0                     if tau>tau1
          where tau*dlam = -lam(0), z(tau) = [x(tau);g(tau)]
          and lam(tau) = [lam_x(tau);lam_g(tau)]
          This gives the following expression for the dual infeasibility
          as a function of tau<=tau1:
            infeas(tau) = g + H*x(tau) + A'*lam_g(tau) + lam_x(tau)
                        = g + H*lam(0) + A'*lam_g(0) + lam_x(0)
                        + tau*H*dz + tau*A'*dlam_g + tau*dlam_x
                        = glag(0) + lam_x(0) + tau*(H*dz + A'*dlam_g + dlam_x)
                        = infeas(0) + tau*tinfeas
            The function is continuous in tau, but tinfeas makes a stepwise
            change when tau=tau1.
          Let us find the largest possible tau, while keeping maximum
          dual infeasibility below e.
        */
      // How long step can we take without exceeding e?
      double tau_k = 0.;
      for (casadi_int j=0; j<n_tau; ++j) {
        // Constraint that we're watching
        i = iw[j];
        // Distance to the next tau (may be zero)
        double dtau = w[i] - tau_k;
        // Check if maximum dual infeasibilty gets exceeded
        bool found_tau = false;
        for (casadi_int k=0; k<nx_ && !found_tau; ++k) {
          if (fabs(infeas[k]+dtau*tinfeas[k])>e) {
            double tau1 = fmax(tau_k - dtau*(infeas[k]/tinfeas[k]), 0.);
            if (tau1<tau) {
              // Smallest tau found so far
              found_tau = true;
              tau = tau1;
              index = -1;
              new_active_set = true;
            }
          }
        }
        // To not allow the active set change if e gets exceeded
        if (found_tau) break;
        // Continue to the next tau
        tau_k = w[i];
        // Update infeasibility
        casadi_axpy(nx_, dtau, tinfeas, infeas);
        // Update the infeasibility tangent for next iteration
        if (i<nx_) {
          // Set a lam_x to zero
          tinfeas[i] -= lam[i];
        } else {
          // Set a lam_a to zero
          for (casadi_int k=at_colind[i-nx_]; k<at_colind[i-nx_+1]; ++k) {
            tinfeas[at_row[k]] -= trans_a[k]*lam[i];
          }
        }
        // Accept the tau, set multiplier to zero or flip sign if equality
        if (i!=index) { // ignore if already taken care of
          new_active_set = true;
          lam[i] = !neverzero[i] ? 0 : lam[i]<0 ? DMIN : -DMIN;
          sprint(msg, sizeof(msg), "Removed %lld", i);
          dlam[i] = 0.;
        }
      }

      // Ignore sign changes if they happen for a full step
      if (tau==1.) index = -1;

      if (verbose_) {
        print("tau = %g\n", tau);
      }

      // Take primal step
      casadi_axpy(nx_, tau, dz, z);

      // Update lam carefully
      for (i=0; i<nx_+na_; ++i) {
        // Get the current sign
        casadi_int s = lam[i]>0. ? 1 : lam[i]<0. ? -1 : 0;
        // Account for sign changes
        if (i==index && s!=sign) {
          sprint(msg, sizeof(msg), "Added %lld (%lld->%lld)", i, s, sign);
          new_active_set = true;
          s = sign;
        }
        // Take step
        lam[i] += tau*dlam[i];
        // Ensure correct sign
        switch (s) {
          case -1: lam[i] = fmin(lam[i], -DMIN); break;
          case  1: lam[i] = fmax(lam[i],  DMIN); break;
          case  0: lam[i] = 0.; break;
        }
      }
    }

    // Calculate optimal cost
    if (f) *f = fk;

    // Get solution
    casadi_copy(z, nx_, x);
    casadi_copy(lam, nx_, lam_x);
    casadi_copy(lam+nx_, na_, lam_a);

    return flag;
  }

} // namespace casadi
