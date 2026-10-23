set minimum-version := "1.55.0"

mod project 'just/project/mod.just'
mod? local 'just/local.just'

default:
    @just --list
