# -*- python -*-

load("@drake//tools/workspace:github.bzl", "github_archive")

def petsc_repository(
        name,
        mirrors = None):
    github_archive(
        name = name,
        repository = "petsc/petsc",
        commit = "v3.16.4",
        sha256 = "c3cbbbf0dd10d58ceb286e28d354233d287c52abf884b50accfb17689ad77613",  # noqa
        build_file = "@drake//tools/workspace/petsc:package.BUILD.bazel",
        mirrors = mirrors,
        patches = [
            # Patch to fix dangerous global state in PETSc.
            "@drake//tools/workspace/petsc:patches/destroy.patch",
            "@drake//tools/workspace/petsc:patches/inherit.patch",
            "@drake//tools/workspace/petsc:patches/matrix.patch",
            "@drake//tools/workspace/petsc:patches/mpi.patch",
            "@drake//tools/workspace/petsc:patches/petscimpl.patch",
            "@drake//tools/workspace/petsc:patches/pname.patch",
            "@drake//tools/workspace/petsc:patches/remove_packages.patch",
            "@drake//tools/workspace/petsc:patches/tagm.patch",
        ],
    )
